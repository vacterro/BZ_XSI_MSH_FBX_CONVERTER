"""Source-to-output fidelity verification (SAIHANDOFF-12 §10, T-13 repair).

Compares a bzconv source manifest (dumped from the parsed Canonical Scene by
`bzconv manifest <file>`) against an exported GLB imported independently via
the glTF evaluator. Reference values come from the PARSED SOURCE, never from
the exported file, so exporter loss is detectable.

T-13 checks: geometry with oriented winding, scale-aware positional
tolerance, per-primitive material bindings, animation clip/track/keyframe
content and effective world matrices (including explicit glTF `matrix`).
T-14 closes two residual false-success holes: material binding identities
are compared by the full ordered per-primitive assignment (and material
properties), and every animation keyframe in every track is checked, not
just the first, middle and last. Feature verdicts distinguish preserved,
intentionally excluded and lost features. Geometry-only profiles are never
labelled full-scene verified.

Usage:
    python tools/source_fidelity_check.py <model.glb> --manifest <manifest.json>

Exit 0 = verified/unverified, 1 = violations, 2 = cannot evaluate.
"""
from __future__ import annotations

import json
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gltf_scene_evaluator as ev

# T-13: absolute floor covers float32 rounding near zero; the scale term
# admits quantisation on large assets without allowing centimetre drift on
# small game details. Upper bound keeps relative error honest.
POS_ABS_TOL = 2e-4
POS_REL = 1e-5
POS_MAX_TOL = 5e-3
UV_TOL = 2e-3
DOT_TOL = 0.999
ANIM_TIME_TOL = 2e-3
ANIM_VALUE_TOL = 3e-3
MATERIAL_PROPS_UNCHECKED = "NOT_INDEPENDENTLY_CHECKED"


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    if m.get("format") != "bzconv-source-manifest":
        raise ValueError("not a bzconv-source-manifest")
    return m


def position_tolerance(manifest):
    """Scale-aware tolerance derived from the source bounding-box diagonal."""
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for node in manifest.get("nodes", []):
        for prim in node.get("primitives", []):
            for corner in prim.get("corners", []):
                for k in range(3):
                    v = float(corner[k])
                    lo[k] = min(lo[k], v)
                    hi[k] = max(hi[k], v)
    if lo[0] == math.inf:
        return POS_ABS_TOL
    diag = math.sqrt(sum((hi[k] - lo[k]) ** 2 for k in range(3)))
    return max(POS_ABS_TOL, min(POS_MAX_TOL, diag * POS_REL))


def face_normal(tri):
    """Oriented (winding-sensitive) face normal, unnormalised."""
    a, b, c = tri
    u = [float(b[k]) - float(a[k]) for k in range(3)]
    v = [float(c[k]) - float(a[k]) for k in range(3)]
    return [u[1] * v[2] - u[2] * v[1],
            u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0]]


def oriented_match(mtri, gtri, tol, expect_flip=False):
    """Cyclic rotations preserve winding; odd permutations reverse it."""
    cycles = ((0, 1, 2), (1, 2, 0), (2, 0, 1))
    used = [False] * 3
    for gi in range(3):
        if used[gi]:
            continue
        for perm in cycles:
            if all(abs(float(mtri[k][c]) - float(gtri[perm[k]][c])) <= tol
                   for k in range(3) for c in range(3)):
                for idx in perm:
                    used[idx] = True
                mn, gn = face_normal(mtri), face_normal(gtri)
                ma = math.sqrt(sum(x * x for x in mn))
                ga = math.sqrt(sum(x * x for x in gn))
                if ma < 1e-10 or ga < 1e-10:
                    return True, True  # degenerate: position only
                dot = sum(mn[k] * gn[k] for k in range(3)) / (ma * ga)
                agrees = dot < -DOT_TOL if expect_flip else dot > DOT_TOL
                return True, agrees
    return False, False


def triangles_of(doc, binchunk, prim):
    """LOCAL-space triangles of one GLB primitive (no world transform)."""
    attrs = prim.get("attributes", {})
    pos = ev.read_accessor(doc, binchunk, attrs["POSITION"])
    idx = prim.get("indices")
    idxs = ev.read_accessor(doc, binchunk, idx) if idx is not None else list(range(len(pos)))
    out = []
    for t in range(0, len(idxs) - 2, 3):
        out.append([[float(c) for c in pos[idxs[t + k]]] for k in range(3)])
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


def det3(m):
    r = [float(m[i][j]) for i in range(3) for j in range(3)]
    return (r[0] * (r[4] * r[8] - r[5] * r[7])
            - r[1] * (r[3] * r[8] - r[5] * r[6])
            + r[2] * (r[3] * r[7] - r[4] * r[6]))


def compare_materials(manifest, doc, report):
    """Per-primitive bindings + material inventory + material properties.

    T-14: check the WHOLE ordered per-primitive assignment, not just that a
    material id exists somewhere in the inventory. Swapping two binding ids,
    or dropping every binding while leaving the material array intact, is a
    fidelity loss. Compare double-sided and texture channels too, so a
    misbound material cannot hide behind a shared name.
    """
    requested = manifest.get("requestedFeatures", {}).get("materials", True)
    src_mats = manifest.get("materials", [])
    glb_mats = doc.get("materials", [])
    if not requested:
        return
    if src_mats and not glb_mats:
        report.error("MATERIALS_LOST",
                     f"source has {len(src_mats)} materials, export has none")
        return
    if len(src_mats) != len(glb_mats):
        report.error("MATERIAL_COUNT",
                     f"{len(glb_mats)} GLB materials vs {len(src_mats)} source materials")

    names = [str(m.get("name", "")) for m in glb_mats]
    for mi, smat in enumerate(src_mats):
        if mi >= len(glb_mats):
            report.error("MATERIAL_MISSING",
                         f"source material {mi} ('{smat.get('name')}') absent from export")
            continue
        gmat = glb_mats[mi]
        want_name = str(smat.get("name", ""))
        got_name = str(gmat.get("name", ""))
        if want_name and got_name and want_name != got_name:
            report.error("MATERIAL_NAME_MISMATCH",
                         f"material {mi}: '{got_name}' vs source '{want_name}'")
        ref = str(smat.get("materialFileRef", ""))
        if ref:
            got_ref = str((gmat.get("extras") or {}).get("bz_materialFile", ""))
            if got_ref != ref:
                report.error("MATERIAL_FILE_REF_MISMATCH",
                             f"material {mi}: '{got_ref}' vs source '{ref}'")
        want_double = smat.get("doubleSided")
        if want_double is not None and bool(gmat.get("doubleSided", False)) != bool(want_double):
            report.error("MATERIAL_PROPERTY_MISMATCH",
                         f"material {mi} ({want_name}): doubleSided differs from source")
        want_tex = smat.get("textures") or {}
        src_refs = {str(v.get("ref", "")) for v in want_tex.values()
                    if isinstance(v, dict) and v.get("ref")}
        # Some source materials carry no per-channel ref while their primitives
        # still reference a .pic/.dds. Include primitive refs so losing every
        # texture channel cannot stay VERIFIED behind an empty material field.
        for node in manifest.get("nodes", []):
            for sprim in node.get("primitives", []):
                if sprim.get("material") == mi:
                    src_refs.update(str(r) for r in sprim.get("textureRefs", []) if r)
        if not src_refs or not manifest.get("requestedFeatures", {}).get("textures", True):
            continue
        glb_refs = {str(v) for v in (gmat.get("extras") or {}).values() if isinstance(v, str)}
        pbr = gmat.get("pbrMetallicRoughness") or {}
        if pbr.get("baseColorTexture"):
            glb_refs.add("__BASE_COLOR_TEXTURE__")
        if gmat.get("normalTexture"):
            glb_refs.add("__NORMAL_TEXTURE__")
        if gmat.get("emissiveTexture"):
            glb_refs.add("__EMISSIVE_TEXTURE__")
        # Source refs are .pic/.dds paths; GLB extras carry resolved paths and
        # images are embedded, so match by channel presence, not raw path.
        if not glb_refs and not requested_texture_channels(doc):
            report.error("MATERIAL_TEXTURE_CHANNELS_LOST",
                         f"material {mi} ({want_name}): source texture channels absent from export")

    ordered = []
    for node in manifest.get("nodes", []):
        for sprim in node.get("primitives", []):
            ordered.append((sprim.get("material"), sprim.get("materialName")))
    actual = []
    for mesh in doc.get("meshes", []):
        for gprim in mesh.get("primitives", []):
            actual.append(gprim.get("material"))
    if len(ordered) != len(actual):
        report.error("MATERIAL_BINDING_COUNT",
                     f"{len(actual)} GLB primitive bindings vs {len(ordered)} source bindings")
    else:
        for pi, ((want_id, want_name), got_id) in enumerate(zip(ordered, actual)):
            if want_id is None or want_id < 0:
                if got_id is not None:
                    report.error("MATERIAL_BINDING_UNEXPECTED",
                                 f"primitive {pi}: export binding {got_id} with no source material")
                continue
            if got_id is None:
                report.error("MATERIAL_BINDING_MISSING",
                             f"primitive {pi}: source material {want_id} ({want_name}) lost")
            elif int(got_id) != int(want_id):
                report.error("MATERIAL_BINDING_MISMATCH",
                             f"primitive {pi}: material {got_id} ('{names[int(got_id)] if 0 <= int(got_id) < len(names) else '?'}') "
                             f"vs source {want_id} ({want_name})")
            elif want_name and 0 <= int(want_id) < len(names) and names[int(want_id)] != want_name:
                report.error("MATERIAL_BINDING_NAME_MISMATCH",
                             f"primitive {pi}: material name '{names[int(want_id)]}' vs source '{want_name}'")


def requested_texture_channels(doc):
    """True when any exported material carries a texture channel or image."""
    if doc.get("images"):
        return True
    return any(m.get("normalTexture") or m.get("emissiveTexture") or
               (m.get("pbrMetallicRoughness") or {}).get("baseColorTexture")
               for m in doc.get("materials", []))


def channel_key(ch):
    tgt = ch.get("target", {})
    return (int(tgt.get("node", -1)), str(tgt.get("path", "")))


def sample_glb_channel(doc, binchunk, anim, ch):
    """Decode sampler input/output into (times, values) lists."""
    smp = anim.get("samplers", [])[ch.get("sampler", -1)]
    times = ev.read_accessor(doc, binchunk, smp["input"])
    vals = ev.read_accessor(doc, binchunk, smp["output"])
    return [float(t) for t in times], [list(v) for v in vals]


def compare_animations(manifest, doc, binchunk, report):
    """Clip identity, track identity and keyframe values (not just counts)."""
    requested = manifest.get("requestedFeatures", {}).get("animations", True)
    src_clips = manifest.get("animations", [])
    glb_anim = doc.get("animations", [])
    if not requested or not src_clips:
        return
    if not glb_anim:
        report.error("ANIMATION_LOST",
                     f"source has {len(src_clips)} animation clips, export has none")
        return
    by_name = {str(a.get("name", "")): a for a in glb_anim}
    for clip in src_clips:
        name = str(clip.get("name", ""))
        if name not in by_name:
            report.error("ANIMATION_CLIP_LOST", f"clip '{name}' absent from export")
            continue
        ganim = by_name[name]
        gchannels = ganim.get("channels", [])
        gmap = {}
        for ch in gchannels:
            gmap.setdefault(channel_key(ch), ch)
        expected = {}
        for tr in clip.get("tracks", []):
            node = int(tr.get("nodeIndex", -1))
            for path in ("translations", "rotations", "scales"):
                keys = tr.get(path) or []
                if not keys:
                    continue
                gpath = {"translations": "translation", "rotations": "rotation",
                         "scales": "scale"}[path]
                expected[(node, gpath)] = (path, keys)
        missing = sorted(set(expected) - set(gmap))
        extra = sorted(set(gmap) - set(expected))
        if missing:
            report.error("ANIMATION_TRACK_LOST", f"clip '{name}': {missing} absent from export")
        if extra:
            report.error("ANIMATION_TRACK_EXTRA", f"clip '{name}': unexpected tracks {extra}")
        fps = float(clip.get("fps", 20.0)) or 20.0
        start = float(clip.get("startFrame", 0.0))
        for key, (path, keys) in expected.items():
            ch = gmap.get(key)
            if ch is None:
                continue
            try:
                gtimes, gvals = sample_glb_channel(doc, binchunk, ganim, ch)
            except Exception as e:
                report.error("ANIMATION_UNREADABLE", f"clip '{name}' {key}: {e}")
                continue
            if len(gtimes) != len(keys) or len(gvals) != len(keys):
                report.error("ANIMATION_KEY_COUNT",
                             f"clip '{name}' {key}: {len(gtimes)} GLB keys vs {len(keys)} source")
                continue
            # T-14: every keyframe in every track. Sampling only first/middle/
            # last let a corrupted intermediate key pass as VERIFIED on real
            # fvtank animation. Timing and value are both source-relative.
            for i, row in enumerate(keys):
                want_t = (float(row[0]) - start) / fps
                if abs(want_t - gtimes[i]) > ANIM_TIME_TOL:
                    report.error("ANIMATION_KEY_TIME",
                                 f"clip '{name}' {key} key {i}: t={gtimes[i]} vs {want_t}")
                    break
                # Source quats are (w,x,y,z); glTF stores (x,y,z,w).
                want_v = ([float(row[2]), float(row[3]), float(row[4]), float(row[1])]
                          if path == "rotations" else [float(x) for x in row[1:]])
                got_v = [float(x) for x in gvals[i]]
                ok = (all(abs(a - b) <= ANIM_VALUE_TOL for a, b in zip(want_v, got_v))
                      if path != "rotations" else
                      (all(abs(a - b) <= ANIM_VALUE_TOL for a, b in zip(want_v, got_v)) or
                       all(abs(-a - b) <= ANIM_VALUE_TOL for a, b in zip(want_v, got_v))))
                if not ok:
                    report.error("ANIMATION_KEY_VALUE",
                                 f"clip '{name}' {key} key {i}: {got_v} vs {want_v}")
                    break


def compare(manifest, doc, binchunk, report):
    """Source manifest vs independently imported GLB.
    Returns (checked_primitive_count, partial_flag)."""
    nodes = doc.get("nodes", [])
    parent = ev.resolve_parents(doc, report)
    world = ev.resolve_world_matrices(nodes, parent)
    tol = position_tolerance(manifest)

    compare_materials(manifest, doc, report)
    compare_animations(manifest, doc, binchunk, report)

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

    for src_node, glb_pair in zip(src_mesh_nodes, glb_nodes):
        glb_ni, glb_node = glb_pair
        glb_mi = glb_node.get("mesh")

        src_wm = src_node.get("worldMatrix")
        glb_wm = world.get(glb_ni)
        # T-13: an explicit glTF matrix is a transform too. Effective world
        # matrix must always match. Skinned bind-space nodes are the one
        # deliberate exception (their geometry is localised separately).
        if src_wm and glb_wm and "skin" not in glb_node:
            wm_bad = any(abs(float(src_wm[i][j]) - float(glb_wm[i][j])) > 1e-3
                         for i in range(4) for j in range(4))
            if wm_bad:
                report.error("MANIFEST_WORLD_MATRIX",
                             f"node {glb_ni} ({src_node.get('name')}) world matrix differs from source")
                partial = True
        checked += 1

        if glb_mi is None or glb_mi >= len(doc.get("meshes", [])):
            report.error("MANIFEST_NODE_MESH", f"node {glb_ni} has no mesh in GLB")
            partial = True
            continue
        glb_prims = doc["meshes"][glb_mi]["primitives"]
        src_prims = src_node.get("primitives", [])
        if len(glb_prims) != len(src_prims):
            report.error("MANIFEST_PRIM_COUNT",
                         f"node {glb_ni}: {len(glb_prims)} GLB primitives vs {len(src_prims)} source")
            partial = True

        expect_flip = False
        if src_wm and glb_wm:
            expect_flip = det3(src_wm) * det3(glb_wm) < 0

        if len(glb_prims) == len(src_prims):
            for pi, sprim in enumerate(src_prims):
                glb_prim = glb_prims[pi]
                try:
                    gtris = triangles_of(doc, binchunk, glb_prim)
                except Exception as e:
                    report.error("MANIFEST_ACC_UNREADABLE", f"node {glb_ni} prim {pi}: {e}")
                    partial = True
                    continue
                raw = sprim.get("corners", [])
                mtris = [raw[i:i + 3] for i in range(0, len(raw) - 2, 3)]
                if len(mtris) != len(gtris):
                    report.error("MANIFEST_TRIANGLE_COUNT",
                                 f"node {glb_ni} prim {pi}: {len(gtris)} GLB tris vs {len(mtris)} source")
                    partial = True
                    continue
                used = [False] * len(gtris)
                reversed_count = 0
                for si, mtri in enumerate(mtris):
                    match = -1
                    agrees = True
                    for gi, gtri in enumerate(gtris):
                        if used[gi]:
                            continue
                        found, ok_winding = oriented_match(mtri, gtri, tol, expect_flip)
                        if found:
                            match = gi
                            agrees = ok_winding
                            break
                    if match < 0:
                        report.error("MANIFEST_GEOMETRY",
                                     f"node {glb_ni} prim {pi}: source triangle {si} not found in GLB")
                        partial = True
                    else:
                        used[match] = True
                        if not agrees:
                            reversed_count += 1
                if reversed_count:
                    report.error("MANIFEST_WINDING_REVERSED",
                                 f"node {glb_ni} prim {pi}: {reversed_count} triangles reversed")
                    partial = True
                checked += 1

                flip_v = bool(src_node.get("uvOriginBottomLeft", True))
                m_uvs = sprim.get("uvCorners") or None
                g_uvs = glb_uv_corners(doc, binchunk, glb_prim)
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
                m_nms = sprim.get("normalCorners") or None
                g_nms = glb_normal_corners(doc, binchunk, glb_prim)
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


def classify(manifest, doc, checked, partial, violations):
    """Feature-level verdict. No full verdict when requested features lost."""
    if violations or partial:
        return "SOURCE_FIDELITY_PARTIAL"
    if checked == 0:
        return "UNVERIFIED"
    req = manifest.get("requestedFeatures", {})
    if req.get("materials", True) and manifest.get("materials") and not doc.get("materials"):
        return "GEOMETRY_FIDELITY_VERIFIED"
    if req.get("materials") is False:
        return "GEOMETRY_FIDELITY_VERIFIED"
    return "SOURCE_FIDELITY_VERIFIED"


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
    fidelity = ev.Report()
    checked, partial = compare(manifest, doc, binchunk, fidelity)
    for code, msg in fidelity.errors:
        print(f"VIOLATION {code}: {msg}")

    struct_errs = list(report.errors)
    violations = list(fidelity.errors)
    print(f"STRUCTURAL: {'PASS' if not struct_errs else 'FAIL'}")
    if struct_errs:
        print("FIDELITY: INVALID (structural violations)")
        return 1

    req = manifest.get("requestedFeatures", {})
    material_state = ("PRESERVED" if doc.get("materials") else
                      "NOT_REQUESTED" if req.get("materials") is False else "LOST")
    anim_state = ("PRESERVED" if doc.get("animations") else
                  "NOT_REQUESTED" if req.get("animations") is False or not manifest.get("animations")
                  else "LOST")
    print(f"MATERIALS: {material_state}")
    print(f"ANIMATIONS: {anim_state}")
    print(f"TEXTURES: {MATERIAL_PROPS_UNCHECKED}")
    print(f"SKINNING: {MATERIAL_PROPS_UNCHECKED}")
    verdict = classify(manifest, doc, checked, partial, violations)
    print(f"FIDELITY: {verdict} ({checked} primitives checked)")
    return 1 if verdict == "SOURCE_FIDELITY_PARTIAL" else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
