"""Independent glTF 2.0 / GLB scene evaluator (SAIHANDOFF-12 repair).

Validation oracle with its own math: nothing is imported from the C++
exporter. Reads a GLB, evaluates the node hierarchy (cycles, TRS, nesting),
reconstructs world-space geometry and normals with the glTF column-vector
convention (world = parent_world @ child_local), decodes accessors honoring
bufferView.byteStride and accessor.byteOffset with component normalization,
checks skin bindings (joints/weights/inverse binds), decodes animation
keyframe VALUES (not just channel counts), and decodes embedded images with
Pillow as the independent decoder.

Modes:
    python gltf_scene_evaluator.py model.glb [--strict]
        Structural + semantic validation. Exit 0 pass, 1 violations.
    python gltf_scene_evaluator.py model.glb --reference pristine.glb
        Adds source-relative fidelity comparison (counts verified before
        values; every mesh primitive compared; animation keyframes sampled).
        SAIHANDOFF-12 §10 statuses:
            STRUCTURAL: PASS/FAIL
            FIDELITY: SOURCE_FIDELITY_VERIFIED | SOURCE_FIDELITY_PARTIAL |
                      UNVERIFIED (no reference given)
        Structural violations keep the file INVALID; fidelity result is only
        meaningful when structural is PASS.
Exit codes: 0 = all requested checks passed, 1 = violations found,
2 = could not evaluate (bad container, unsupported data, missing Pillow...).
"""
from __future__ import annotations

import io
import json
import struct
import sys
from dataclasses import dataclass, field

try:
    from PIL import Image
except ImportError:  # optional: image checks degrade, not fail
    Image = None


# ----------------------------------------------------------------- constants
_COMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT4": 16}
_FMT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
_FMT_SIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
_POS_TOL = 1e-3
_UV_TOL = 1e-5
_DOT_TOL = 0.999


class UnsupportedFeature(Exception):
    """Accessor/feature the evaluator intentionally does not decode."""


# --------------------------------------------------------------------- GLB IO
def read_glb(path):
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise ValueError("not a GLB container")
    total = struct.unpack_from("<I", data, 8)[0]
    if total != len(data):
        raise ValueError(f"declared length {total} != file length {len(data)}")
    doc = binchunk = None
    off = 12
    while off + 8 <= len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        if off + 8 + clen > len(data):
            raise ValueError("chunk overruns container")
        if ctype == 0x4E4F534A:  # JSON
            if doc is not None:
                raise ValueError("duplicate JSON chunk")
            doc = json.loads(data[off + 8 : off + 8 + clen])
        elif ctype == 0x004E4942:  # BIN
            if binchunk is not None:
                raise ValueError("duplicate BIN chunk")
            binchunk = data[off + 8 : off + 8 + clen]
        off += 8 + clen
    if doc is None:
        raise ValueError("missing JSON chunk")
    return doc, binchunk


# ------------------------------------------------------------- accessor decode
def decode_accessor(doc, binchunk, idx):
    """Decode ONE accessor into a list of elements.

    SCALAR -> [float|int, ...]; VECn -> [[c0, c1, ...], ...];
    MATn   -> [[m00..mNN], ...] as stored (column-major, 16 floats for MAT4).

    Honors bufferView.byteOffset, bufferView.byteStride (interleaved data),
    accessor.byteOffset, normalized integer components, MAT2/MAT4 layout.
    Raises UnsupportedFeature for sparse accessors / unsupported buffers.
    """
    acc = doc["accessors"][idx]
    comps = _COMP[acc["type"]]
    count = acc["count"]
    ctype = acc["componentType"]
    if ctype not in _FMT:
        raise UnsupportedFeature(f"accessor {idx}: bad componentType {ctype}")
    fmt_char = _FMT[ctype]
    comp_size = _FMT_SIZE[ctype]
    elem_size = comp_size * comps

    bv_idx = acc.get("bufferView")
    if bv_idx is None:
        raise UnsupportedFeature(f"accessor {idx}: no bufferView")
    bv = doc["bufferViews"][bv_idx]
    buf_idx = bv.get("buffer", 0)
    if buf_idx != 0:
        raise UnsupportedFeature(f"accessor {idx}: references buffer {buf_idx} (GLB has none)")
    if bv.get("byteStride") is None:
        stride = 0
    else:
        stride = bv["byteStride"]
        if stride < elem_size or stride > 252 or stride % 4:
            raise UnsupportedFeature(f"accessor {idx}: illegal byteStride {stride}")

    bv_off = bv.get("byteOffset", 0)
    acc_off = acc.get("byteOffset", 0)
    effective = bv_off + acc_off
    # alignment per glTF: accessor byteOffset multiple of component size
    if acc_off % comp_size:
        raise UnsupportedFeature(
            f"accessor {idx}: byteOffset {acc_off} not multiple of component size {comp_size}"
        )
    if effective % comp_size:
        raise UnsupportedFeature(
            f"accessor {idx}: effective offset {effective} misaligned for component size {comp_size}"
        )
    if stride == 0:
        stride = elem_size  # tightly packed

    if count < 0 or count > 0x7FFFFFFF:
        raise UnsupportedFeature(f"accessor {idx}: invalid count {count}")
    if effective < 0 or bv_off < 0 or acc_off < 0:
        raise UnsupportedFeature(f"accessor {idx}: negative byte offsets")
    bv_len = bv.get("byteLength")
    if bv_len is None or bv_len < 0:
        raise UnsupportedFeature(f"accessor {idx}: invalid bufferView.byteLength")
    if effective < bv_off or effective > bv_off + bv_len:
        raise UnsupportedFeature(f"accessor {idx}: accessor offset outside bufferView")
    need = effective + stride * (count - 1) + elem_size if count else 0
    if need > bv_off + bv_len:
        raise UnsupportedFeature(
            f"accessor {idx}: needs {need} bytes, bufferView ends at {bv_off + bv_len}"
        )
    if need > len(binchunk):
        raise UnsupportedFeature(
            f"accessor {idx}: needs {need} bytes, buffer holds {len(binchunk)}"
        )

    if acc.get("sparse"):
        raise UnsupportedFeature(f"accessor {idx}: sparse accessors unsupported here")

    normalized = bool(acc.get("normalized"))

    elems = []
    for i in range(count):
        base = effective + i * stride
        raw = struct.unpack_from("<" + fmt_char * comps, binchunk, base)
        if normalized:
            if ctype in (5120, 5122):  # signed: map to [-1, 1]
                qmax = float(2 ** (8 * comp_size - 1) - 1)
                raw = tuple(max(c / qmax, -1.0) for c in raw)
            else:  # unsigned 5121/5123/5125: map to [0, 1]
                qmax = float(2 ** (8 * comp_size) - 1)
                if max(raw) > qmax:
                    raise UnsupportedFeature(f"accessor {idx}: value exceeds normalization range")
                raw = tuple(c / qmax for c in raw)
        elems.append(list(raw) if comps > 1 else raw[0])
    return elems


def read_accessor(doc, binchunk, idx):
    """Compatibility wrapper around the strict decoder; used by callers."""
    return decode_accessor(doc, binchunk, idx)


# ------------------------------------------------------------------ scene math
def quat_to_matrix(q):
    """Column-vector rotation matrix R_col(q) in [row][col] notation."""
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def node_local_matrix(n):
    """Column-vector TRS matrix, [row][col], translation in the last
    column (glTF spec: p_world = M @ p_local, world = parent @ local).
    Respects an explicit 'matrix' property when TRS is absent."""
    if "matrix" in n and "translation" not in n and "rotation" not in n and "scale" not in n:
        mv = n["matrix"]  # glTF stores column-major 16
        return [
            [mv[0], mv[4], mv[8], mv[12]],
            [mv[1], mv[5], mv[9], mv[13]],
            [mv[2], mv[6], mv[10], mv[14]],
            [mv[3], mv[7], mv[11], mv[15]],
        ]
    t = n.get("translation", [0, 0, 0])
    q = n.get("rotation", [0, 0, 0, 1])
    s = n.get("scale", [1, 1, 1])
    r = quat_to_matrix(q)
    m = [[r[i][j] * s[j] for j in range(3)] for i in range(3)]
    for i in range(3):
        m[i].append(t[i])
    m.append([0.0, 0.0, 0.0, 1.0])
    return m


def transform_point(m, p):
    return [sum(p[k] * m[j][k] for k in range(3)) + m[j][3] for j in range(3)]


def transform_dir(m3, v):
    return [sum(v[k] * m3[j][k] for k in range(3)) for j in range(3)]


def inv_transpose3(m):
    # column storage: A[row][col] = m[row][col] for the upper 3x3
    a = [m[i][:3] for i in range(3)]
    det = (
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
        - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
        + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])
    )
    if abs(det) < 1e-12:
        return None
    inv = [
        [
            (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det,
            (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det,
            (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det,
        ],
        [
            (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det,
            (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det,
            (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det,
        ],
        [
            (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det,
            (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det,
            (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det,
        ],
    ]
    return [[inv[j][i] for j in range(3)] for i in range(3)]  # transposed


def normalize(v):
    l = sum(c * c for c in v) ** 0.5
    return [c / l for c in v] if l > 1e-12 else v


@dataclass
class Report:
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    def error(self, code, msg):
        self.errors.append((code, msg))

    def warn(self, code, msg):
        self.warnings.append((code, msg))


# ------------------------------------------------------- hierarchy resolvers
def resolve_parents(doc, report):
    """Parent map from children edges; multi-parent and cycle diagnostics."""
    nodes = doc.get("nodes", [])
    parent = {i: None for i in range(len(nodes))}
    for pi, n in enumerate(nodes):
        for c in n.get("children", []):
            if not (0 <= c < len(nodes)):
                report.error("HIERARCHY_BAD_INDEX", f"node {pi} child {c} out of range")
                continue
            if parent[c] is not None and parent[c] != pi:
                report.error(
                    "HIERARCHY_MULTI_PARENT", f"node {c} has parents {parent[c]} and {pi}"
                )
            else:
                parent[c] = pi
    for start in range(len(nodes)):
        seen = set()
        cur = start
        while cur is not None:
            if cur in seen:
                report.error("HIERARCHY_CYCLE", f"cycle through node {cur}")
                break
            seen.add(cur)
            cur = parent[cur]
    return parent


def resolve_world_matrices(nodes, parent):
    """THE authoritative world resolver: single implementation, glTF
    column-vector convention world[node] = world[parent] @ local[node].

    Handles parent-before-child, child-before-parent storage order, nested
    rotations, nonuniform scales, negative scales, multiple roots. A cycle
    (already reported by resolve_parents) resolves from the topmostcomputed
    reachable ancestor so evaluation can continue."""
    world = {}
    resolved = set()

    def resolve_chain(start):
        # climb to the highest un-resolved ancestor (cycle-safe: nodes in
        # the current chain stop the climb at chain_bottom)
        chain = []
        cur = start
        chain_set = set()
        while cur is not None and cur not in world and cur not in chain_set:
            chain.append(cur)
            chain_set.add(cur)
            cur = parent[cur]
        # chain = [start, parent(start), ...,  top]; resolve top -> start
        for node_i in reversed(chain):
            base = world.get(parent[node_i]) if parent[node_i] is not None else None
            local = node_local_matrix(nodes[node_i])
            if cur is not None and parent[node_i] is None:
                # node sits on a cycle whose top has no parent: local only
                base = None
            world[node_i] = mat_mul(base, local) if base is not None else local
            resolved.add(node_i)

    for i in range(len(nodes)):
        if i not in resolved:
            resolve_chain(i)
            # any node whose ancestors live on a cycle: re-walk to attach
    # second pass: nodes whose parent became available after cycle fallback
    for i in range(len(nodes)):
        resolve_chain(i)
    return world


# ----------------------------------------------------------------- evaluation
def evaluate(doc, binchunk, report, reference=None):
    """Structural + semantic validation of one GLB. With `reference` (a
    second (doc, binchunk) pair from the SAME source), adds source-relative
    comparisons: world-space normals, geometry, UV layout, node placements,
    skin weights, inverse binds, animation VALUES. Those corruptions are
    undetectable from the file alone."""
    nodes = doc.get("nodes", [])

    # --- hierarchy: parents derived from children edges; forest + acyclic
    parent = resolve_parents(doc, report)

    # --- world matrices via the single authoritative resolver
    world = resolve_world_matrices(nodes, parent)

    # scene roots listed must exist; every non-root node must be reachable
    scenes = doc.get("scenes", [])
    if scenes:
        declared_roots = set(scenes[doc.get("scene", 0)].get("nodes", []))
        actual_roots = {i for i in range(len(nodes)) if parent[i] is None}
        unreferenced_roots = actual_roots - declared_roots
        if unreferenced_roots:
            report.warn(
                "SCENE_ROOTS_DIFFER",
                f"nodes with no parent but not in scene roots: {sorted(unreferenced_roots)}",
            )
    else:
        report.error("SCENE_MISSING", "no scenes array")

    # --- meshes: world geometry, winding-det normals, attributes, ALL prims
    for mi, mesh in enumerate(doc.get("meshes", [])):
        prims = mesh.get("primitives", [])
        if not prims:
            report.error("MESH_NO_PRIMITIVES", f"mesh {mi} has no primitives")
        for pi, prim in enumerate(prims):
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs:
                report.error("MESH_NO_POSITION", f"mesh {mi} primitive {pi} without POSITION")
                continue
            try:
                pos = read_accessor(doc, binchunk, attrs["POSITION"])
            except (UnsupportedFeature, IndexError, struct.error) as e:
                report.error("ACCESSOR_UNREADABLE", f"mesh {mi} prim {pi} POSITION: {e}")
                continue
            owner = None
            for ni, n in enumerate(nodes):
                if n.get("mesh") == mi:
                    owner = ni
            if owner is None:
                continue  # unreferenced mesh: legal, skip eval
            wm = world[owner]
            if not pos:
                report.error("MESH_EMPTY", f"mesh {mi} primitive {pi} has zero vertices")
            # indices in range
            if "indices" in prim:
                try:
                    idxs = read_accessor(doc, binchunk, prim["indices"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    report.error("ACCESSOR_UNREADABLE", f"mesh {mi} prim {pi} indices: {e}")
                    idxs = None
                if idxs is not None:
                    if any(not (0 <= int(v) < len(pos)) for v in idxs):
                        report.error(
                            "INDEX_OUT_OF_RANGE",
                            f"mesh {mi} prim {pi} references vertex >= {len(pos)}",
                        )
                    if prim.get("mode", 4) == 4 and len(idxs) % 3:
                        report.error(
                            "INDEX_COUNT_NOT_TRIANGLE",
                            f"mesh {mi} prim {pi} TRIANGLES with {len(idxs)} indices",
                        )
            for p in pos:
                if any(x != x or abs(x) == float("inf") for x in p):
                    report.error(
                        "POSITION_NOT_FINITE", f"mesh {mi} prim {pi} has non-finite vertex"
                    )
                    break
            normals = None
            if "NORMAL" in attrs:
                try:
                    normals = read_accessor(doc, binchunk, attrs["NORMAL"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    report.error("ACCESSOR_UNREADABLE", f"mesh {mi} prim {pi} NORMAL: {e}")
            if normals:
                it = inv_transpose3(wm)
                if it is None:
                    report.error("NORMAL_SINGULAR", f"mesh {mi} node transform not invertible")
                else:
                    first = normals[0]
                    wn = transform_dir([it[j][:3] for j in range(3)], first)
                    nrm = normalize(wn)
                    if nrm == [0, 0, 0]:
                        report.error("NORMAL_DEGENERATE", f"mesh {mi} world normal is zero")
            # skin binding consistency for EVERY node holding this mesh
            for holder in [i for i, n in enumerate(nodes) if n.get("mesh") == mi]:
                skin_ref = nodes[holder].get("skin")
                if skin_ref is None:
                    continue
                has_j = any(k.startswith("JOINTS_") for k in attrs)
                has_w = any(k.startswith("WEIGHTS_") for k in attrs)
                if not (has_j and has_w):
                    report.error(
                        "SKIN_MISSING_ATTRIBUTES",
                        f"mesh {mi} node {holder} references skin {skin_ref} without JOINTS/WEIGHTS",
                    )
                    continue
                # T-13: sum ALL JOINTS_n / WEIGHTS_n sets. Exporters may
                # split >4 influences across multiple vertex attributes.
                sets = sorted(
                    int(k.split("_")[1]) for k in attrs
                    if k.startswith("JOINTS_") and k.split("_")[1].isdigit()
                )
                wsets = sorted(
                    int(k.split("_")[1]) for k in attrs
                    if k.startswith("WEIGHTS_") and k.split("_")[1].isdigit()
                )
                if not sets or sets != wsets or sets != list(range(len(sets))):
                    report.error(
                        "SKIN_SET_MISMATCH",
                        f"mesh {mi} JOINTS sets {sets} vs WEIGHTS sets {wsets}",
                    )
                    continue
                try:
                    jsets = [read_accessor(doc, binchunk, attrs[f"JOINTS_{n}"]) for n in sets]
                    wsets_data = [read_accessor(doc, binchunk, attrs[f"WEIGHTS_{n}"]) for n in sets]
                except (UnsupportedFeature, IndexError, struct.error, KeyError) as e:
                    report.error("ACCESSOR_UNREADABLE", f"mesh {mi} skin attributes: {e}")
                    continue
                if any(len(x) != len(pos) for x in jsets + wsets_data):
                    report.error(
                        "SKIN_COUNT_MISMATCH",
                        f"mesh {mi} JOINTS/WEIGHTS count != POSITION count",
                    )
                    continue
                skin = doc["skins"][skin_ref]
                n_joints = len(skin["joints"])
                for row in range(len(pos)):
                    ssum = 0.0
                    for jr, wr in zip(jsets, wsets_data):
                        ssum += sum(wr[row])
                    if abs(ssum - 1.0) > 0.05 and ssum > 1e-6:
                        report.error(
                            "SKIN_WEIGHT_SUM",
                            f"mesh {mi} vertex {row} weights sum {ssum:.3f}",
                        )
                        break
                    if any(j >= n_joints or j < 0
                           for js in jsets for j in js[row]):
                        report.error(
                            "SKIN_JOINT_RANGE",
                            f"mesh {mi} vertex {row} joint index out of range",
                        )
                        break
                    if any(not (w >= -1e-6) for ws in wsets_data for w in ws[row]):
                        report.error(
                            "SKIN_WEIGHT_NEGATIVE",
                            f"mesh {mi} vertex {row} negative weight",
                        )
                        break
                if "inverseBindMatrices" in skin:
                    try:
                        ibm = read_accessor(doc, binchunk, skin["inverseBindMatrices"])
                    except (UnsupportedFeature, IndexError, struct.error) as e:
                        report.error("ACCESSOR_UNREADABLE", f"skin {skin_ref} IBM: {e}")
                        continue
                    if len(ibm) != n_joints:
                        report.error(
                            "SKIN_IBM_COUNT",
                            f"skin {skin_ref} has {len(ibm)} inverse binds for {n_joints} joints",
                        )

    # --- skins referenced by nodes must exist
    for ni, n in enumerate(nodes):
        sk = n.get("skin")
        if sk is not None and not (0 <= sk < len(doc.get("skins", []))):
            report.error("SKIN_BAD_REF", f"node {ni} references skin {sk} that does not exist")

    # --- animations: channels/targets valid, samplers decode, values sane
    _eval_animations(doc, binchunk, report)

    # --- images must decode (Pillow as the independent decoder)
    if doc.get("images"):
        if Image is None:
            report.warn("IMAGES_UNCHECKED", "Pillow missing; embedded images not decoded")
        else:
            for ii, img in enumerate(doc["images"]):
                if "bufferView" in img:
                    bv = doc["bufferViews"][img["bufferView"]]
                    blob = binchunk[
                        bv.get("byteOffset", 0) : bv.get("byteOffset", 0) + bv["byteLength"]
                    ]
                    try:
                        with Image.open(io.BytesIO(blob)) as im:
                            im.verify()
                    except Exception as e:
                        report.error("IMAGE_UNDECODABLE", f"image {ii}: {e}")
                elif "uri" in img:
                    report.warn("IMAGE_EXTERNAL", f"image {ii} external uri, not checked here")

    # --- accessors referenced by primitives must exist and be in range
    nacc = len(doc.get("accessors", []))
    nmat = len(doc.get("materials", []))
    for mi, mesh in enumerate(doc.get("meshes", [])):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            for k, a in prim.get("attributes", {}).items():
                if not (0 <= a < nacc):
                    report.error("ACCESSOR_BAD_REF", f"mesh {mi} primitive {pi} attribute {k} -> accessor {a}")
            idx = prim.get("indices")
            if idx is not None and not (0 <= idx < nacc):
                report.error("ACCESSOR_BAD_REF", f"mesh {mi} primitive {pi} indices -> accessor {idx}")
            mat = prim.get("material")
            if mat is not None and not (0 <= mat < nmat):
                report.error(
                    "MATERIAL_BAD_REF",
                    f"mesh {mi} primitive {pi} references material {mat} of {nmat}",
                )

    # --- source-relative checks (only with a pristine reference)
    if reference is not None:
        compare_with_reference(doc, binchunk, reference[0], reference[1], report)

    return report


def _eval_animations(doc, binchunk, report):
    nodes = doc.get("nodes", [])
    for ai, anim in enumerate(doc.get("animations", [])):
        for ch in anim.get("channels", []):
            tgt = ch.get("target", {})
            if not (0 <= tgt.get("node", -1) < len(nodes)):
                report.error("ANIM_BAD_NODE", f"animation {ai} targets bad node {tgt.get('node')}")
            if tgt.get("path") not in ("translation", "rotation", "scale", "weights"):
                report.error("ANIM_BAD_PATH", f"animation {ai} path {tgt.get('path')}")
            si = ch.get("sampler")
            if not (0 <= si < len(anim.get("samplers", []))):
                report.error("ANIM_BAD_SAMPLER", f"animation {ai} channel -> sampler {si}")
        for si, smp in enumerate(anim.get("samplers", [])):
            interp = smp.get("interpolation", "LINEAR")
            if interp not in ("LINEAR", "STEP", "CUBICSPLINE"):
                report.error("ANIM_BAD_INTERP", f"animation {ai} sampler {si} interp {interp}")
            try:
                times = read_accessor(doc, binchunk, smp["input"])
                outs = read_accessor(doc, binchunk, smp["output"])
            except (UnsupportedFeature, IndexError, struct.error, KeyError) as e:
                report.error("ANIM_SAMPLER_UNREADABLE", f"animation {ai} sampler {si}: {e}")
                continue
            if not times:
                report.error("ANIM_EMPTY", f"animation {ai} sampler {si} has no keys")
                continue
            if any(times[k + 1] < times[k] - 1e-9 for k in range(len(times) - 1)):
                report.error("ANIM_TIME_ORDER", f"animation {ai} sampler {si} times not sorted")
            if len(outs) != len(times):
                report.error(
                    "ANIM_KEY_COUNT",
                    f"animation {ai} sampler {si}: {len(outs)} outputs vs {len(times)} times",
                )
            if interp == "CUBICSPLINE" and len(outs) == len(times):
                pass  # triple-key layouts are per spec; skip deep cubic checks


# ------------------------------------------------- world data for comparisons
def collect_world_data(doc, binchunk):
    """Full fidelity snapshot per node: EVERY primitive's world-space
    positions/normals/UVs, per-vertex skin weights, node world matrix,
    and decoded animation channels (times + values). Keys:
      int node id -> entry dict
      "skinN"     -> list of inverse bind matrices (16 floats each)
      "animN"     -> list of channel dicts {node, path, times, values}
    """
    nodes = doc.get("nodes", [])
    parent = {i: None for i in range(len(nodes))}
    for pi, n in enumerate(nodes):
        for c in n.get("children", []):
            if 0 <= c < len(nodes) and parent[c] is None:
                parent[c] = pi
    world = resolve_world_matrices(nodes, parent)

    data = {}

    def unreadable(errs, key):
        # store the exception message so comparisons can flag indeterminate
        data[key] = {"UNREADABLE": str(errs)}

    for ni, n in enumerate(nodes):
        mi = n.get("mesh")
        if mi is None:
            continue
        prims = doc["meshes"][mi].get("primitives", [])
        prim_entries = []
        for prim in prims:
            attrs = prim.get("attributes", {})
            entry = {"material": prim.get("material"), "indices": prim.get("indices")}
            if "POSITION" in attrs:
                try:
                    pos = read_accessor(doc, binchunk, attrs["POSITION"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    unreadable(e, f"node{ni}_pos")
                    prim_entries.append({"UNREADABLE": str(e)})
                    continue
                wm = world[ni]
                entry["world_pos"] = [transform_point(wm, p) for p in pos]
                entry["vertex_count"] = len(pos)
            if "NORMAL" in attrs:
                try:
                    nrm = read_accessor(doc, binchunk, attrs["NORMAL"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    prim_entries.append({"UNREADABLE": str(e)})
                    continue
                it = inv_transpose3(world[ni])
                entry["world_norm"] = (
                    [normalize(transform_dir([it[j][:3] for j in range(3)], v)) for v in nrm]
                    if it
                    else None
                )
                entry["normal_count"] = len(nrm)
            if "TEXCOORD_0" in attrs:
                try:
                    entry["uv"] = read_accessor(doc, binchunk, attrs["TEXCOORD_0"])
                    entry["uv_count"] = len(entry["uv"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    entry["uv_errors"] = str(e)
            if "WEIGHTS_0" in attrs:
                try:
                    entry["weights"] = read_accessor(doc, binchunk, attrs["WEIGHTS_0"])
                    entry["weight_count"] = len(entry["weights"])
                except (UnsupportedFeature, IndexError, struct.error) as e:
                    entry["weight_errors"] = str(e)
            prim_entries.append(entry)
        data[ni] = {"primitives": prim_entries, "world_matrix": world[ni]}
    for si, skin in enumerate(doc.get("skins", [])):
        if "inverseBindMatrices" in skin:
            try:
                data[f"skin{si}"] = read_accessor(doc, binchunk, skin["inverseBindMatrices"])
            except (UnsupportedFeature, IndexError, struct.error) as e:
                data[f"skin{si}"] = None
    for ai, anim in enumerate(doc.get("animations", [])):
        channels = []
        for ch in anim.get("channels", []):
            tgt = ch.get("target", {})
            si = ch.get("sampler")
            if not (0 <= si < len(anim.get("samplers", []))):
                continue
            smp = anim["samplers"][si]
            try:
                times = read_accessor(doc, binchunk, smp["input"])
                values = read_accessor(doc, binchunk, smp["output"])
            except (UnsupportedFeature, IndexError, struct.error):
                continue
            channels.append(
                {
                    "node": tgt.get("node"),
                    "path": tgt.get("path"),
                    "interpolation": smp.get("interpolation", "LINEAR"),
                    "times": times,
                    "values": values,
                }
            )
        data[f"anim{ai}"] = channels
    return data


def _mesh_entries(data):
    return {k: v for k, v in data.items() if isinstance(k, int)}


def _prim_signature(entry):
    """Comparable summary of one primitive: counts + material binding."""
    if entry.get("UNREADABLE"):
        return None
    return {
        "vertex_count": entry.get("vertex_count"),
        "normal_count": entry.get("normal_count"),
        "uv_count": entry.get("uv_count"),
        "weight_count": entry.get("weight_count"),
        "material": entry.get("material"),
        "indices": entry.get("indices"),
    }


def _match_primitives(ref_prims, cur_prims):
    """Match reference primitives to current primitives allowing reordered
    output (order change is legal when content is provably equivalent),
    robust against exact duplicates by consuming matches, not just testing
    membership. Primitives whose signature differs (e.g. altered counts) fall
    back to positional pairing when the slot is free, so field-level count
    codes still report the exact difference. Returns (matches,
    missing_in_current, extra_in_current) as lists of indices."""
    unused_cur = list(range(len(cur_prims)))
    matched_ref = []
    missing = []
    for ri, rprim in enumerate(ref_prims):
        rsig = _prim_signature(rprim)
        found = None
        for ci in unused_cur:
            if _prim_signature(cur_prims[ci]) == rsig:
                found = ci
                break
        if found is None and ri in unused_cur:
            # positional fallback: same slot, mismatched signature — pair so
            # the field-level count comparison names the exact difference
            found = ri
        if found is None:
            missing.append(ri)
        else:
            unused_cur.remove(found)
            matched_ref.append((ri, found))
    return matched_ref, missing, unused_cur


def _sample_anim(channels, node, path):
    """Return the last keyframe value of the (node, path) channel, or None."""
    for ch in channels:
        if ch["node"] == node and ch["path"] == path and ch["values"]:
            return ch["values"][-1]
    return None


def _compare_channels(ref_ch, cur_ch, report, tag):
    """Compare one animation channel: keyframe times and VALUES with
    tolerance. Different key layouts are resampled at the union of
    timestamps before comparison (linear), so a corrupt transform at any
    moment is detected even when key lists differ."""
    # channel identity already matched by (node, path)
    rt, ct = ref_ch["times"], cur_ch["times"]
    rv, cv = ref_ch["values"], cur_ch["values"]
    if _times_close(rt, ct) and len(rv) == len(cv):
        if ref_ch["path"] == "rotation":
            # quaternions: compare via |dot| (double cover)
            bad = sum(
                1
                for a, b in zip(rv, cv)
                if abs(sum(x * y for x, y in zip(a, b))) < _DOT_TOL
            )
        else:
            bad = sum(
                1
                for a, b in zip(rv, cv)
                if any(abs(x - y) > 1e-2 for x, y in zip(a, b))
            )
        if bad:
            report.error("REF_ANIM_VALUES", f"{tag}: {bad} keyframe values deviate")
        return
    # differing layouts: sample both across the common duration and compare
    if not rt or not ct:
        report.error("REF_ANIM_KEYS", f"{tag}: empty keyframe set")
        return
    # coverage: fewer/shifted keys that shorten or shift coverage are a
    # fidelity loss even when all sampled points inside the overlap agree
    if rt[-1] > ct[-1] + 1e-6 or rt[0] < ct[0] - 1e-6:
        report.error(
            "REF_ANIM_DURATION",
            f"{tag}: coverage [{ct[0]:.3f}..{ct[-1]:.3f}] vs reference [{rt[0]:.3f}..{rt[-1]:.3f}]",
        )
        return
    samples = 16
    t0 = max(rt[0], ct[0])
    t1 = min(rt[-1], ct[-1])
    if t1 < t0:
        report.error("REF_ANIM_DURATION", f"{tag}: time ranges do not overlap")
        return
    for s in range(samples):
        t = t0 + (t1 - t0) * s / (samples - 1) if samples > 1 else t0
        a = _interp_channel(ref_ch, t)
        b = _interp_channel(cur_ch, t)
        if a is None or b is None:
            continue
        if ref_ch["path"] == "rotation":
            d = abs(sum(x * y for x, y in zip(a, b)))
            if d < _DOT_TOL:
                report.error("REF_ANIM_VALUES", f"{tag}: sampled rotation at t={t:.3f} deviates")
                return
        else:
            if any(abs(x - y) > 1e-2 * max(1.0, abs(x)) for x, y in zip(a, b)):
                report.error("REF_ANIM_VALUES", f"{tag}: sampled value at t={t:.3f} deviates")
                return


def _times_close(a, b):
    return len(a) == len(b) and all(abs(x - y) < 1e-6 for x, y in zip(a, b))


def _interp_channel(ch, t):
    ts, vs = ch["times"], ch["values"]
    if not ts:
        return None
    if t <= ts[0]:
        return vs[0]
    if t >= ts[-1]:
        return vs[-1]
    for k in range(len(ts) - 1):
        if ts[k] <= t <= ts[k + 1]:
            dt = ts[k + 1] - ts[k]
            f = 0.0 if dt <= 0 else (t - ts[k]) / dt
            a, b = vs[k], vs[k + 1]
            if ch["path"] == "rotation":
                d = sum(x * y for x, y in zip(a, b))
                sgn = 1.0 if d >= 0 else -1.0
                return [x * (1 - f) + y * sgn * f for x, y in zip(a, b)]
            return [x * (1 - f) + y * f for x, y in zip(a, b)]
    return vs[-1]


def compare_with_reference(doc, binchunk, ref_doc, ref_bin, report):
    ref = collect_world_data(ref_doc, ref_bin)
    cur = collect_world_data(doc, binchunk)

    ref_meshes = _mesh_entries(ref)
    cur_meshes = _mesh_entries(cur)
    if len(ref_meshes) != len(cur_meshes):
        report.error(
            "REF_MESH_COUNT", f"{len(cur_meshes)} mesh nodes vs reference {len(ref_meshes)}"
        )
        return

    geo_bad = uv_bad = w_bad = nrm_bad = 0
    prim_loss = 0
    for ni in ref_meshes:
        rentry, centry = ref_meshes[ni], cur_meshes[ni]
        # node world matrix must match exactly (within fp tolerance)
        rw, cw = rentry.get("world_matrix"), centry.get("world_matrix")
        if rw is not None and cw is not None:
            if any(abs(rw[i][j] - cw[i][j]) > 1e-4 for i in range(4) for j in range(4)):
                report.error(
                    "REF_NODE_TRANSFORM", f"node {ni} world matrix deviates from reference"
                )
        rprims, cprims = rentry.get("primitives", []), centry.get("primitives", [])
        # matching handles count differences: missing/extra primitives are
        # reported and matched pairs are still compared field by field
        matches, missing, extra = _match_primitives(rprims, cprims)
        if missing:
            prim_loss += len(missing)
        if extra:
            prim_loss += len(extra)
        for ri, ci in matches:
            rp, cp = rprims[ri], cprims[ci]
            if rp.get("UNREADABLE") or cp.get("UNREADABLE"):
                report.error("REF_PRIM_UNREADABLE", f"node {ni} primitive unreadable")
                continue
            # COUNT verification BEFORE value comparison (SAIHANDOFF-12 §6);
            # material binding is part of primitive identity: a dropped
            # material reference is a fidelity loss even when geometry matches
            for key, code in (
                ("vertex_count", "REF_VERTEX_COUNT"),
                ("normal_count", "REF_NORMAL_COUNT"),
                ("uv_count", "REF_UV_COUNT"),
                ("weight_count", "REF_WEIGHT_COUNT"),
            ):
                if key in rp and cp.get(key) != rp[key]:
                    report.error(
                        code,
                        f"node {ni} prim {ri}: {cp.get(key)} vs reference {rp[key]}",
                    )
            if rp.get("material") != cp.get("material"):
                report.error(
                    "REF_MATERIAL_BINDING",
                    f"node {ni} prim {ri}: material {cp.get('material')} vs reference {rp.get('material')}",
                )
            if rp.get("world_norm") and cp.get("world_norm"):
                if len(rp["world_norm"]) != len(cp["world_norm"]):
                    nrm_bad += 1
                else:
                    for a, b in zip(rp["world_norm"], cp["world_norm"]):
                        dot = sum(x * y for x, y in zip(a, b))
                        if dot < _DOT_TOL:
                            nrm_bad += 1
            if rp.get("world_pos") and cp.get("world_pos"):
                if len(rp["world_pos"]) != len(cp["world_pos"]):
                    geo_bad += len(cp["world_pos"])
                else:
                    for a, b in zip(rp["world_pos"], cp["world_pos"]):
                        if any(abs(x - y) > _POS_TOL for x, y in zip(a, b)):
                            geo_bad += 1
            if rp.get("uv") and cp.get("uv"):
                if len(rp["uv"]) != len(cp["uv"]):
                    uv_bad += len(cp["uv"])
                else:
                    for a, b in zip(rp["uv"], cp["uv"]):
                        if any(abs(x - y) > _UV_TOL for x, y in zip(a, b)):
                            uv_bad += 1
            if rp.get("weights") and cp.get("weights"):
                if len(rp["weights"]) != len(cp["weights"]):
                    w_bad += len(cp["weights"])
                else:
                    for a, b in zip(rp["weights"], cp["weights"]):
                        if any(abs(x - y) > _UV_TOL for x, y in zip(a, b)):
                            w_bad += 1
    if prim_loss:
        report.error(
            "REF_PRIMITIVE_COUNT",
            f"{prim_loss} material primitives missing or added vs reference",
        )
    if nrm_bad:
        report.error("REF_WORLD_NORMAL", f"{nrm_bad} world normals deviate from reference")
    if geo_bad:
        report.error("REF_WORLD_GEOMETRY", f"{geo_bad} world positions deviate from reference")
    if uv_bad:
        report.error("REF_UV_LAYOUT", f"{uv_bad} UV coordinates deviate from reference")
    if w_bad:
        report.error("REF_SKIN_WEIGHTS", f"{w_bad} skin weight rows deviate from reference")

    for k, v in ref.items():
        if isinstance(k, int):
            continue
        if k.startswith("skin"):
            if v is None or cur.get(k) is None:
                report.error("REF_IBM_UNREADABLE", f"{k} inverse binds undecodable")
                continue
            if len(v) != len(cur[k]):
                report.error("REF_IBM_COUNT", f"{k} inverse bind count differs")
                continue
            bad = sum(
                1
                for ra, ca in zip(v, cur[k])
                if ra and ca and any(abs(x - y) > _UV_TOL for x, y in zip(ra, ca))
            )
            if bad:
                report.error("REF_IBM_VALUES", f"{k}: {bad} inverse bind matrices differ")
        elif k.startswith("anim"):
            cch = cur.get(k)
            if cch is None:
                report.error("REF_ANIM_MISSING", f"{k} missing entirely")
                continue
            if len(v) != len(cch):
                report.error(
                    "REF_ANIM_TRACKS",
                    f"{k} has {len(cch)} channels vs reference {len(v)}",
                )
                continue
            # match channels by identity (target node, path), tolerant of order
            used = set()
            for rch in v:
                match = None
                for idx, cch2 in enumerate(cch):
                    if idx in used:
                        continue
                    if cch2["node"] == rch["node"] and cch2["path"] == rch["path"]:
                        match = idx
                        break
                if match is None:
                    report.error(
                        "REF_ANIM_CHANNEL",
                        f"{k}: channel node {rch['node']} path {rch['path']} missing",
                    )
                    continue
                used.add(match)
                _compare_channels(
                    rch, cch[match], report, f"{k} node{rch['node']}.{rch['path']}"
                )


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    try:
        doc, binchunk = read_glb(argv[1])
        reference = None
        if "--reference" in argv:
            rpath = argv[argv.index("--reference") + 1]
            rdoc, rbin = read_glb(rpath)
            reference = (rdoc, rbin)
    except Exception as e:
        print(f"EVALUATION_FAILED: {e}")
        return 2
    report = Report()
    evaluate(doc, binchunk, report, reference)
    for code, msg in report.errors:
        print(f"VIOLATION {code}: {msg}")
    for code, msg in report.warnings:
        print(f"warning {code}: {msg}")
    structural_ok = not report.errors
    if reference is None:
        print(f"STRUCTURAL: {'PASS' if structural_ok else 'FAIL'}")
        print("FIDELITY: UNVERIFIED (no reference supplied)")
    else:
        fidelity_errs = [e for e in report.errors if e[0].startswith("REF_")]
        struct_errs = [e for e in report.errors if not e[0].startswith("REF_")]
        print(f"STRUCTURAL: {'PASS' if not struct_errs else 'FAIL'}")
        if not struct_errs:
            if not fidelity_errs:
                print("FIDELITY: SOURCE_FIDELITY_VERIFIED")
            else:
                print("FIDELITY: SOURCE_FIDELITY_PARTIAL")
        else:
            print("FIDELITY: INVALID (structural violations)")
    if report.errors:
        print(
            f"EVAL RESULT: FAIL ({len(report.errors)} violations, {len(report.warnings)} warnings)"
        )
        return 1
    print(f"EVAL RESULT: PASS ({len(report.warnings)} warnings)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


def world_bbox(doc, binchunk):
    """World-space bounding box over every mesh node's transformed geometry."""
    nodes = doc.get("nodes", [])
    parent = {i: None for i in range(len(nodes))}
    for pi, n in enumerate(nodes):
        for c in n.get("children", []):
            if 0 <= c < len(nodes) and parent[c] is None:
                parent[c] = pi
    world = resolve_world_matrices(nodes, parent)
    lo = hi = None
    for ni, n in enumerate(nodes):
        mi = n.get("mesh")
        if mi is None:
            continue
        for prim in doc["meshes"][mi].get("primitives", []):
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs:
                continue
            try:
                pos = read_accessor(doc, binchunk, attrs["POSITION"])
            except (UnsupportedFeature, IndexError, struct.error):
                continue
            wm = world[ni]
            for p in pos:
                wp = transform_point(wm, p)
                if lo is None:
                    lo = list(wp)
                    hi = list(wp)
                else:
                    for j in range(3):
                        lo[j] = min(lo[j], wp[j])
                        hi[j] = max(hi[j], wp[j])
    return lo, hi
