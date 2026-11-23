"""T-13 RED/GREEN witnesses for the source fidelity checker.

Synthetic manifests + GLBs prove that material loss, animation loss, winding
reversal, explicit-matrix tampering and large positional drift can no longer
produce a false SOURCE_FIDELITY_VERIFIED. A geometry-only profile is labelled
GEOMETRY_FIDELITY_VERIFIED with MATERIALS: NOT_REQUESTED, never full verified.

Run: python tools/test_source_fidelity_oracle.py
"""
from __future__ import annotations

import copy
import json
import pathlib
import struct
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gltf_scene_evaluator as ev
import source_fidelity_check as sf

FAILURES = []


def check(name, ok, detail=""):
    print(f"[fidelity] {name}: {'PASS' if ok else 'FAIL'}" + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        FAILURES.append(name)


def minimal_scene():
    """One node, one mesh, one triangle, one material; TRS identity."""
    s = ev.Report
    class B:
        pass
    b = B()
    b.nodes = [{"name": "root", "mesh": 0, "translation": [0, 0, 0],
                "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}]
    b.buffer = bytearray()
    b.buffer_views = []
    b.accessors = []
    b.animations = []

    def acc(flat, comp_type, comps):
        fmt = {5126: "f", 5125: "I"}[comp_type]
        while len(b.buffer) % 4:
            b.buffer.append(0)
        off = len(b.buffer)
        b.buffer += struct.pack(f"<{len(flat)}{fmt}", *flat)
        b.buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(flat) * 4})
        b.accessors.append({"bufferView": len(b.buffer_views) - 1, "componentType": comp_type,
                            "count": len(flat) // comps, "type": {1: "SCALAR", 3: "VEC3", 4: "VEC4"}[comps]})
        return len(b.accessors) - 1

    pos = acc([0, 0, 0, 1, 0, 0, 0, 1, 0], 5126, 3)
    idx = acc([0, 1, 2], 5125, 1)
    b.meshes = [{"primitives": [{"attributes": {"POSITION": pos}, "indices": idx, "material": 0}]}]
    b.materials = [{"name": "mat0", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}]
    b.scenes = [{"nodes": [0]}]

    def finish():
        while len(b.buffer) % 4:
            b.buffer.append(0)
        doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": b.scenes,
               "nodes": b.nodes, "meshes": b.meshes, "accessors": b.accessors,
               "bufferViews": b.buffer_views, "buffers": [{"byteLength": len(b.buffer)}],
               "materials": b.materials}
        if b.animations:
            doc["animations"] = b.animations
        return doc, bytes(b.buffer)
    return b, finish


def manifest(profile="full-scene", materials=True, animations=True):
    f = {"materials": materials, "textures": materials, "animations": animations}
    m = {"format": "bzconv-source-manifest", "version": 2,
         "requestedProfile": profile, "requestedFeatures": f,
         "materials": [{"name": "mat0", "textures": {}}],
         "animations": [],
         "nodes": [{"name": "root", "worldMatrix": [[1, 0, 0, 0], [0, 1, 0, 0],
                                                     [0, 0, 1, 0], [0, 0, 0, 1]],
                    "primitives": [{"material": 0, "materialName": "mat0",
                                    "corners": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]}]}]}
    return m


def run_check(manifest_obj, doc, binary):
    rep = ev.Report()
    checked, partial = sf.compare(manifest_obj, doc, binary, rep)
    return rep, checked, partial


def verdict(manifest_obj, doc, binary):
    """Full main() classification without files: mirror source_fidelity_check."""
    rep2 = ev.Report()
    checked, partial = sf.compare(manifest_obj, doc, binary, rep2)
    errs = list(rep2.errors)
    if errs:
        return "PARTIAL_OR_FAIL", errs
    if partial:
        return "PARTIAL", []
    if checked == 0:
        return "UNVERIFIED", []
    req = manifest_obj.get("requestedFeatures", {})
    if req.get("materials") and not doc.get("materials"):
        return "GEOMETRY_ONLY", []
    if req.get("animations") and manifest_obj.get("animations") and not doc.get("animations"):
        return "PARTIAL_OR_FAIL", []
    return "VERIFIED", []


def main():
    b, finish = minimal_scene()
    doc, binary = finish()
    m = manifest()

    # Baseline: full scene with matching material must verify.
    v, errs = verdict(m, doc, binary)
    check("baseline full scene verified", v == "VERIFIED", f"{v} {errs}")

    # 1. Material reference changed while both materials exist.
    d2, bin2 = copy.deepcopy(doc), binary
    d2["materials"].append({"name": "mat1", "pbrMetallicRoughness": {}})
    d2["meshes"][0]["primitives"][0]["material"] = 1
    v, errs = verdict(m, d2, bin2)
    check("material mismatch detected", v == "PARTIAL_OR_FAIL" and any("MATERIAL" in c for c, _ in errs), f"{v} {errs}")

    # 2. Materials disabled in export: label geometry-only, never full verified.
    d3, bin3 = copy.deepcopy(doc), binary
    d3.pop("materials")
    d3["meshes"][0]["primitives"][0].pop("material")
    v, errs = verdict(m, d3, bin3)
    check("material loss not full verified", v in ("PARTIAL_OR_FAIL", "GEOMETRY_ONLY"), f"{v} {errs}")

    # Geometry-only profile with no materials is honest geometry verdict.
    mg = manifest(profile="geom-only", materials=False)
    v, errs = verdict(mg, d3, bin3)
    check("geometry-only profile classified", v == "VERIFIED" and not mg["requestedFeatures"]["materials"], f"{v} {errs}")

    # 3. Animation dropped from full-scene export.
    ma = manifest()
    ma["animations"] = [{"name": "WALK", "tracks": [{"nodeIndex": 0, "translations": [[0, 0, 0, 0], [1, 0, 0, 0]]}]}]
    d4, bin4 = copy.deepcopy(doc), binary
    d4["animations"] = [{"name": "WALK", "channels": [], "samplers": []}]
    v, errs = verdict(ma, d4, bin4)
    check("animation loss detected", v == "PARTIAL_OR_FAIL", f"{v} {errs}")

    # 4. Reversed triangle winding (indices swapped).
    d5, bin5 = copy.deepcopy(doc), binary
    idx_acc = d5["accessors"][1]
    bv = d5["bufferViews"][idx_acc["bufferView"]]
    ba = bytearray(bin5)
    struct.pack_into("<III", ba, bv["byteOffset"], 0, 2, 1)
    v, errs = verdict(m, d5, bytes(ba))
    check("reversed winding detected", v == "PARTIAL_OR_FAIL" and any(c in ("WINDING", "MANIFEST_WINDING_REVERSED", "MANIFEST_GEOMETRY") for c, _ in errs), f"{v} {errs}")

    # 5. Explicit matrix translation by 100 units.
    d6, bin6 = copy.deepcopy(doc), binary
    d6["nodes"][0].pop("translation")
    d6["nodes"][0].pop("rotation")
    d6["nodes"][0].pop("scale")
    d6["nodes"][0]["matrix"] = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 0, 0, 1]
    rep, checked, partial = run_check(m, d6, bin6)
    check("explicit matrix checked", any(c in ("MANIFEST_WORLD_MATRIX", "MANIFEST_GEOMETRY") for c, _ in rep.errors), f"{[c for c, _ in rep.errors]}")

    # 6. One-vertex displacement +0.01 with new tight tolerance.
    d7, bin7 = copy.deepcopy(doc), binary
    pos_acc = d7["accessors"][0]
    bv = d7["bufferViews"][pos_acc["bufferView"]]
    ba = bytearray(bin7)
    old = struct.unpack_from("<f", ba, bv["byteOffset"] + 3 * 4)[0]
    struct.pack_into("<f", ba, bv["byteOffset"] + 3 * 4, old + 0.01)
    rep, checked, partial = run_check(m, d7, bytes(ba))
    check("small displacement detected", any(c == "MANIFEST_GEOMETRY" for c, _ in rep.errors), f"{[c for c, _ in rep.errors]}")

    # T-14 witness 1: swap primitive material bindings while both material
    # definitions remain. Checking only inventory/names misses this.
    d8 = copy.deepcopy(doc)
    d8["materials"].append({"name": "mat1"})
    d8["meshes"][0]["primitives"][0]["material"] = 1
    rep, _, _ = run_check(m, d8, binary)
    check("material binding swap detected",
          any(c == "MATERIAL_BINDING_MISMATCH" for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")

    # T-14 witness 2: drop every per-primitive binding but keep the material
    # inventory. A name-only checker still says VERIFIED.
    d9 = copy.deepcopy(doc)
    d9["meshes"][0]["primitives"][0].pop("material", None)
    rep, _, _ = run_check(m, d9, binary)
    check("material binding removal detected",
          any(c == "MATERIAL_BINDING_MISSING" for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")

    # T-14 witness 4: source material has texture channels; export drops every
    # texture channel while keeping geometry, material binding and inventory.
    dtex = copy.deepcopy(doc)
    dtex.pop("images", None)
    for gmat in dtex["materials"]:
        gmat.pop("pbrMetallicRoughness", None)
        gmat.pop("normalTexture", None)
        gmat.pop("emissiveTexture", None)
    mtex = manifest()
    mtex["requestedFeatures"]["textures"] = True
    mtex["materials"][0]["textures"] = {"diffuse": {"ref": "source.pic"}}
    mtex["nodes"][0]["primitives"][0]["textureRefs"] = ["source.pic"]
    rep, _, _ = run_check(mtex, dtex, binary)
    check("texture channel loss detected",
          any(c == "MATERIAL_TEXTURE_CHANNELS_LOST" for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")

    # T-14 witness 3: mutate a MIDDLE animation key. First/middle/last
    # sampling on real fvtank accepted this corruption.
    # Accessors 2/3 are appended after the existing POSITION/indices pair.
    keys = [[0.0, 0, 0, 0], [0.05, 0, 0, 0], [0.1, 0, 0, 0], [0.15, 0, 0, 0], [0.2, 0, 0, 0]]
    b, finish = minimal_scene()
    while len(b.buffer) % 4:
        b.buffer.append(0)
    koff = len(b.buffer)
    kb = bytearray()
    for row in keys:
        kb += struct.pack("<f", row[0])
    b.buffer += kb
    b.buffer_views.append({"buffer": 0, "byteOffset": koff, "byteLength": len(kb)})
    b.accessors.append({"bufferView": len(b.buffer_views) - 1, "componentType": 5126,
                        "count": len(keys), "type": "SCALAR"})
    voff = len(b.buffer)
    vb = bytearray()
    for row in keys:
        vb += struct.pack("<3f", *row[1:])
    b.buffer += vb
    b.buffer_views.append({"buffer": 0, "byteOffset": voff, "byteLength": len(vb)})
    b.accessors.append({"bufferView": len(b.buffer_views) - 1, "componentType": 5126,
                        "count": len(keys), "type": "VEC3"})
    b.animations = [{
        "name": "WALK",
        "samplers": [{"input": 2, "output": 3, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}],
    }]
    d10, bin10 = finish()
    ma = manifest()
    ma["animations"] = [{"name": "WALK", "fps": 20.0, "startFrame": 0.0,
                         "tracks": [{"nodeIndex": 0, "translations":
                                     [[0, 0, 0, 0], [1, 0, 0, 0], [2, 0, 0, 0],
                                      [3, 0, 0, 0], [4, 0, 0, 0]]}]}]
    rep, _, _ = run_check(ma, d10, bin10)
    check("animation baseline verified", not rep.errors, f"{[c for c, _ in rep.errors]}")
    ba10 = bytearray(bin10)
    # Key index 1 of 5: the pre-fix checker sampled only {0, 2, 4}, so this
    # intermediate corruption stayed VERIFIED. The fix checks every key.
    struct.pack_into("<3f", ba10, voff + 1 * 12, 0.5, 0, 0)
    rep, _, _ = run_check(ma, d10, bytes(ba10))
    check("middle animation key mutation detected",
          any(c == "ANIMATION_KEY_VALUE" for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")

    print()
    if FAILURES:
        print(f"FIDELITY ORACLE RESULT: {len(FAILURES)} FAILED: {FAILURES}")
        return 1
    print("FIDELITY ORACLE RESULT: ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
