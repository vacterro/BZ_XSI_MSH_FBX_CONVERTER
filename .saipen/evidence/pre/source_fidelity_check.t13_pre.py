"""Source-to-output fidelity verification (SAIHANDOFF-12 §10).

Compares a bzconv source manifest (dumped from the parsed Canonical Scene by
`bzconv manifest <file>`) against an exported GLB imported independently via
the repaired glTF evaluator. The reference values come from the PARSED
SOURCE, never from the exported file, so exporter loss is detectable.

The comparison is LOCAL-to-LOCAL: the manifest stores exactly the positions
the exporter intends to write (model-space meshes already localized with the
exporter's own rule), and the GLB stores node-local positions. No world
transforms on either side at comparison time.

Usage:
    python tools/source_fidelity_check.py <model.glb> --manifest <manifest.json>

Manifest schema (bzconv manifest <file>):
    nodes[]: {name, parent, worldMatrix (glTF column-vector, [row][col])?,
              primitives[]: {material, corners=[flat x,y,z, 3 per triangle]}}
    animations[]: {name, trackCount}

Result lines:
    STRUCTURAL: PASS/FAIL
    FIDELITY: SOURCE_FIDELITY_VERIFIED | SOURCE_FIDELITY_PARTIAL | UNVERIFIED

Exit 0 = verified/unverified, 1 = violations, 2 = cannot evaluate.
"""
from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gltf_scene_evaluator as ev

POS_TOL = 2e-2      # world units; float32 rounding dominates
UV_TOL = 2e-3
DOT_TOL = 0.999


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    if m.get("format") != "bzconv-source-manifest":
        raise ValueError("not a bzconv-source-manifest")
    return m


def tri_match(mtri, gtri, tol=POS_TOL):
    """Order-insensitive corner match of one LOCAL-space triangle."""
    used = [False] * 3
    for corner in mtri:
        target = [float(x) for x in corner]
        hit = False
        for gi in range(3):
            if used[gi]:
                continue
            if all(abs(target[c] - gtri[gi][c]) <= tol for c in range(3)):
                used[gi] = True
                hit = True
                break
        if not hit:
            return False
    return True


def triangles_of(doc, binchunk, prim):
    """LOCAL-space triangles of one GLB primitive (no world transform):
    the GLB stores node-local positions and so does the manifest."""
    attrs = prim.get("attributes", {})
    pos = ev.read_accessor(doc, binchunk, attrs["POSITION"])
    idx = prim.get("indices")
    idxs = ev.read_accessor(doc, binchunk, idx) if idx is not None else list(range(len(pos)))
    out = []
    for t in range(0, len(idxs) - 2, 3):
        tri = []
        for k in range(3):
            p = pos[idxs[t + k]]
            tri.append([float(c) for c in p])
        out.append(tri)
    return out


def glb_uv_corners(doc, binchunk, prim):
    attrs = prim.get("attributes", {})
    if "TEXCOORD_0" not in attrs:
        return None
    uv = ev.read_accessor(doc, binchunk, attrs["TEXCOORD_0"])
    idx = prim.get("indices")
    if idx is None:
        return [list(u) for u in uv]
    idxs = ev.read_accessor(doc, binchunk, idx)
    if any(int(i) >= len(uv) for i in idxs):
        return None
    return [list(uv[i]) for i in idxs]


def glb_normal_corners(doc, binchunk, prim):
    attrs = prim.get("attributes", {})
    if "NORMAL" not in attrs:
        return None
    nrm = ev.read_accessor(doc, binchunk, attrs["NORMAL"])
    idx = prim.get("indices")
    if idx is None:
        return [list(n) for n in nrm]
    idxs = ev.read_accessor(doc, binchunk, idx)
    if any(int(i) >= len(nrm) for i in idxs):
        return None
    return [list(nrm[i]) for i in idxs]


def compare(manifest, doc, binchunk, report):
    """Source manifest vs independently imported GLB. Returns
    (checked_primitive_count, partial_flag)."""
    nodes = doc.get("nodes", [])
    parent = ev.resolve_parents(doc, report)
    world = ev.resolve_world_matrices(nodes, parent)

    src_mesh_nodes = [n for n in manifest.get("nodes", []) if n.get("primitives")]
    glb_nodes = [(i, n) for i, n in enumerate(nodes) if n.get("mesh") is not None]
    checked = 0
    partial = False

    src_mesh_count = len(src_mesh_nodes)
    if len(doc.get("meshes", [])) != src_mesh_count:
        report.error("MANIFEST_MESH_COUNT",
                     f"{len(doc.get('meshes', []))} GLB meshes vs {src_mesh_count} source meshes")
        partial = True
    if len(glb_nodes) != src_mesh_count:
        report.error("MANIFEST_NODE_COUNT",
                     f"{len(glb_nodes)} GLB mesh nodes vs {src_mesh_count} source")
        partial = True

    # skinned (bind-space) meshes keep model-space coordinates under an
    # identity glTF node: their node world matrices are not comparable 1:1.
    # Node order is the canonical order on both sides (exporter preserves
    # canonical node order); compare pairwise, LOCAL-to-LOCAL.
    for src_node, glb_pair in zip(src_mesh_nodes, glb_nodes):
        glb_ni, glb_node = glb_pair
        glb_mi = glb_node.get("mesh")

        # non-skinned nodes: the resolved GLB world matrix must reproduce the
        # source world matrix. Skinned meshes sit under a deliberate identity
        # node (bind-space geometry), so their node matrices are not 1:1.
        src_wm = src_node.get("worldMatrix")
        glb_wm = world.get(glb_ni)
        if src_wm and glb_wm and "skin" not in glb_node and glb_node.get("matrix") is None:
            wm_bad = any(abs(float(src_wm[i][j]) - float(glb_wm[i][j])) > 1e-3
                         for i in range(4) for j in range(4))
            if wm_bad:
                report.error("MANIFEST_WORLD_MATRIX",
                             f"node {glb_ni} ({src_node.get('name')}) world matrix differs from source")
                partial = True
        checked += 1  # node-level check counts once per node

        src_prims = src_node.get("primitives", [])
        if glb_mi is None or glb_mi >= len(doc.get("meshes", [])):
            report.error("MANIFEST_NODE_MESH", f"node {glb_ni} has no mesh in GLB")
            partial = True
            continue
        glb_prims = doc["meshes"][glb_mi]["primitives"]
        if len(glb_prims) != len(src_prims):
            report.error("MANIFEST_PRIM_COUNT",
                         f"node {glb_ni}: {len(glb_prims)} GLB primitives vs {len(src_prims)} source")
            partial = True

        if len(glb_prims) == len(src_prims):
            for pi, sprim in enumerate(src_prims):
                if pi >= len(glb_prims):
                    break
                glb_prim = doc["meshes"][glb_mi]["primitives"][pi]
                try:
                    gtris = triangles_of(doc, binchunk, glb_prim)
                except Exception as e:
                    report.error("MANIFEST_ACC_UNREADABLE", f"node {glb_ni} prim {pi}: {e}")
                    partial = True
                    continue
                raw = sprim.get("corners", [])
                # flat corner list: 3 entries per triangle
                mtris = [raw[i:i + 3] for i in range(0, len(raw) - 2, 3)]
                if len(mtris) != len(gtris):
                    report.error("MANIFEST_TRIANGLE_COUNT",
                                 f"node {glb_ni} prim {pi}: {len(gtris)} GLB tris vs {len(mtris)} source")
                    partial = True
                    continue
                used = [False] * len(gtris)
                for si, mtri in enumerate(mtris):
                    match = -1
                    for gi, gtri in enumerate(gtris):
                        if used[gi]:
                            continue
                        if tri_match(mtri, gtri):
                            match = gi
                            break
                    if match < 0:
                        report.error("MANIFEST_GEOMETRY",
                                     f"node {glb_ni} prim {pi}: source triangle {si} not found in GLB")
                        partial = True
                    else:
                        used[match] = True
                checked += 1

                # per-corner UV comparison (corner-aligned with triangles);
                # GLB convention flips V, source canonical UVs are bottom-left
                flip_v = bool(src_node.get("uvOriginBottomLeft", True))
                m_uvs = sprim.get("uvCorners") or None
                g_uvs = glb_uv_corners(doc, binchunk, doc["meshes"][glb_mi]["primitives"][pi])
                if m_uvs and g_uvs is None:
                    report.error("MANIFEST_UV_MISSING",
                                 f"node {glb_ni} prim {pi}: UVs in source, absent in GLB")
                    partial = True
                elif m_uvs and g_uvs is not None:
                    if len(m_uvs) != len(g_uvs):
                        report.error("MANIFEST_UV_COUNT",
                                     f"node {glb_ni} prim {pi}: {len(g_uvs)} GLB UV corners vs {len(m_uvs)} source")
                        partial = True
                    else:
                        bad_uv = 0
                        for mu, gu in zip(m_uvs, g_uvs):
                            gu_f = [float(gu[0]), 1.0 - float(gu[1])] if flip_v \
                                   else [float(c) for c in gu]
                            if any(abs(a - c) > UV_TOL for a, c in zip(mu, gu_f)):
                                bad_uv += 1
                        if bad_uv:
                            report.error("MANIFEST_UV_CORNERS",
                                         f"node {glb_ni} prim {pi}: {bad_uv} UV corners deviate")
                            partial = True
                # per-corner normals (direction compare; exporter may re-normalize)
                m_nms = sprim.get("normalCorners") or None
                g_nms = glb_normal_corners(doc, binchunk, doc["meshes"][glb_mi]["primitives"][pi])
                if m_nms and g_nms is None:
                    report.error("MANIFEST_NORMAL_MISSING",
                                 f"node {glb_ni} prim {pi}: normals in source, absent in GLB")
                    partial = True
                elif m_nms and g_nms:
                    if len(m_nms) != len(g_nms):
                        report.error("MANIFEST_NORMAL_COUNT",
                                     f"node {glb_ni} prim {pi}: normal corner count differs")
                        partial = True
                    else:
                        bad = 0
                        for mn, gn in zip(m_nms, g_nms):
                            gn_n = ev.normalize([float(x) for x in gn])
                            dn = sum(float(x) * gn_n[k] for k, x in enumerate(mn))
                            if dn < DOT_TOL:
                                bad += 1
                        if bad:
                            report.error("MANIFEST_NORMAL_CORNERS",
                                         f"node {glb_ni} prim {pi}: {bad} normal corners deviate")
                            partial = True
    return checked, partial


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    glb_path = argv[1]
    manifest_path = None
    if "--manifest" in argv:
        manifest_path = argv[argv.index("--manifest") + 1]
    if manifest_path is None:
        print("STRUCTURAL: (run gltf_scene_evaluator.py for that)")
        print("FIDELITY: UNVERIFIED (unverified — no --manifest given)")
        return 0
    manifest = load_manifest(manifest_path)
    try:
        doc, binchunk = ev.read_glb(glb_path)
    except Exception as e:
        print(f"EVALUATION_FAILED: {e}")
        return 2

    report = ev.Report()
    ev.evaluate(doc, binchunk, report)
    report2 = ev.Report()
    checked, partial = compare(manifest, doc, binchunk, report2)
    for code, msg in report2.errors:
        print(f"VIOLATION {code}: {msg}")

    struct_errs = list(report.errors)
    fidelity_errs = list(report2.errors)
    print(f"STRUCTURAL: {'PASS' if not struct_errs else 'FAIL'}")
    if struct_errs:
        print("FIDELITY: INVALID (structural violations)")
        return 1
    if not fidelity_errs:
        if partial:
            print(f"FIDELITY: SOURCE_FIDELITY_PARTIAL ({checked} primitives checked)")
            return 0
        if checked == 0:
            print("FIDELITY: UNVERIFIED (nothing comparable)")
            return 0
        print(f"FIDELITY: SOURCE_FIDELITY_VERIFIED ({checked} primitives checked)")
        return 0
    print(f"FIDELITY: SOURCE_FIDELITY_PARTIAL ({checked} primitives checked, "
          f"{len(fidelity_errs)} violations)")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
