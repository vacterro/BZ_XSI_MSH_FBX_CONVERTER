"""Synthetic RED/GREEN oracle tests for gltf_scene_evaluator (SAIHANDOFF-12).

Builds minimal-but-valid GLB scenes in memory with hand-computed expected
world coordinates, then asserts the evaluator:
  A. computes glTF-correct world transforms (column-vector convention,
     world = parent @ local) across the SAIHANDOFF-12 §4 matrix,
  B. decodes accessors honoring byteOffset/byteStride/normalization/MAT4,
  C. FAILS every corrupted variant (vertex loss, primitive loss, animation
     value corruption) and PASSES the pristine self-comparison.

Run: python tools/test_evaluator_oracle.py
Exit 0 = all green; 1 = at least one case failed.
"""
import copy
import struct
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gltf_scene_evaluator as ev

FAILURES = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[oracle] {name}: {status}" + (f" ({detail})" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)


# ------------------------------------------------------------------ builder
class Scene:
    """Minimal glTF builder: pack float32/uint32/ushort arrays into one buffer."""

    FMT = {5126: "f", 5125: "I", 5123: "H", 5121: "B", 5122: "h", 5120: "b"}
    ALIGN = {5126: 4, 5125: 4, 5123: 2, 5122: 2, 5121: 1, 5120: 1}
    TYPE = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}

    def __init__(self):
        self.nodes = []
        self.meshes = []
        self.buffer = bytearray()
        self.buffer_views = []
        self.accessors = []
        self.animations = []
        self.scenes_nodes = []

    def add_accessor(self, data, comp_type, comp_count, normalized=False):
        fmt = self.FMT[comp_type]
        blob = struct.pack(f"<{len(data)}{fmt}", *data)
        while len(self.buffer) % self.ALIGN[comp_type]:
            self.buffer.append(0)
        off = len(self.buffer)
        self.buffer += blob
        self.buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(blob)})
        self.accessors.append(
            {
                "bufferView": len(self.buffer_views) - 1,
                "componentType": comp_type,
                "count": len(data) // comp_count,
                "type": self.TYPE[comp_count],
                "normalized": normalized,
            }
        )
        return len(self.accessors) - 1

    def add_mesh(self, positions, indices=None, normals=None, material=None):
        attrs = {"POSITION": self.add_accessor([c for p in positions for c in p], 5126, 3)}
        if normals:
            attrs["NORMAL"] = self.add_accessor([c for n in normals for c in n], 5126, 3)
        prim = {"attributes": attrs, "mode": 4}
        if indices is not None:
            prim["indices"] = self.add_accessor(indices, 5125, 1)
        if material is not None:
            prim["material"] = material
        self.meshes.append({"primitives": [prim]})
        return len(self.meshes) - 1

    def node(self, **kw):
        self.nodes.append(kw)
        return len(self.nodes) - 1

    def link(self, parent_i, child_i):
        self.nodes[parent_i].setdefault("children", []).append(child_i)

    def finish(self):
        while len(self.buffer) % 4:
            self.buffer.append(0)
        doc = {
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": self.scenes_nodes}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "accessors": self.accessors,
            "bufferViews": self.buffer_views,
            "buffers": [{"byteLength": len(self.buffer)}],
        }
        if self.animations:
            doc["animations"] = self.animations
        return doc, bytes(self.buffer)


def add_animation(scene, node, path, times, values, interpolation="LINEAR"):
    ti = scene.add_accessor(times, 5126, 1)
    n_comp = {"translation": 3, "rotation": 4, "scale": 3}[path]
    vi = scene.add_accessor([c for v in values for c in v], 5126, n_comp)
    if not scene.animations:
        scene.animations.append({"samplers": [], "channels": []})
    anim = scene.animations[0]
    anim["samplers"].append({"input": ti, "output": vi, "interpolation": interpolation})
    anim["channels"].append(
        {"sampler": len(anim["samplers"]) - 1, "target": {"node": node, "path": path}}
    )


Q90Z = (0.0, 0.0, 0.7071067811865476, 0.7071067811865476)


# ------------------------------------------------------- §4 world-matrix cases
def run_world_matrix_cases():
    def world_check(name, nodes_spec, points, expected):
        s = Scene()
        chain = [s.node(**nd) for nd in nodes_spec]
        for p, c in zip(chain, chain[1:]):
            s.link(p, c)
        mesh_i = s.add_mesh(points, indices=[0, 0, 0])
        s.nodes[chain[-1]]["mesh"] = mesh_i
        s.scenes_nodes = [chain[0]]
        doc, _ = s.finish()
        parent = ev.resolve_parents(doc, ev.Report())
        world = ev.resolve_world_matrices(doc["nodes"], parent)
        got = ev.transform_point(world[chain[-1]], points[0])
        check(f"world/{name}", all(abs(g - e) < 1e-6 for g, e in zip(got, expected)),
              f"got {got}, expected {expected}")

    # 1. parent translation + child translation
    world_check("parent_T_child_T", [{"translation": [1, 2, 3]}, {"translation": [4, 5, 6]}],
                [[0, 0, 0]], [5, 7, 9])
    # 2. THE handoff case: parent rotZ+90, child T(1,0,0): origin -> (0,1,0)
    world_check("parent_R90z_child_T", [{"rotation": list(Q90Z)}, {"translation": [1, 0, 0]}],
                [[0, 0, 0]], [0, 1, 0])
    # 3. parent scale (2,3,4) + child rotZ90: vertex (1,0,0) -> (0,3,0)
    world_check("parent_S_child_R", [{"scale": [2, 3, 4]}, {"rotation": list(Q90Z)}],
                [[1, 0, 0]], [0, 3, 0])
    # 4. parent rotZ90 + child scale (2,3,4): vertex (1,1,1) -> (-3,2,4)
    world_check("parent_R_child_S", [{"rotation": list(Q90Z)}, {"scale": [2, 3, 4]}],
                [[1, 1, 1]], [-3, 2, 4])
    # 5. three-level T(1,0,0) -> Rz90 -> T(1,0,0): origin -> (1,1,0)
    world_check("three_level",
                [{"translation": [1, 0, 0]}, {"rotation": list(Q90Z)}, {"translation": [1, 0, 0]}],
                [[0, 0, 0]], [1, 1, 0])
    # 6. mirrored parent scale (-1,1,1) + child T(1,0,0): origin -> (-1,0,0)
    world_check("mirrored_parent", [{"scale": [-1, 1, 1]}, {"translation": [1, 0, 0]}],
                [[0, 0, 0]], [-1, 0, 0])

    # 7. arbitrary storage order: child stored BEFORE its parent
    s = Scene()
    child = s.node(translation=[1, 0, 0])
    parent = s.node(rotation=list(Q90Z))
    s.link(parent, child)
    mesh_i = s.add_mesh([[0, 0, 0]], indices=[0, 0, 0])
    s.nodes[child]["mesh"] = mesh_i
    s.scenes_nodes = [parent]
    doc, _ = s.finish()
    pmap = ev.resolve_parents(doc, ev.Report())
    world = ev.resolve_world_matrices(doc["nodes"], pmap)
    got = ev.transform_point(world[child], [0, 0, 0])
    check("world/child_before_parent_storage",
          all(abs(g - e) < 1e-6 for g, e in zip(got, [0, 1, 0])), f"got {got}")

    # 8. multiple roots, independent subtrees
    s = Scene()
    r1, c1 = s.node(translation=[1, 0, 0]), s.node(translation=[1, 0, 0])
    r2, c2 = s.node(translation=[0, 5, 0]), s.node(translation=[0, 1, 0])
    s.link(r1, c1)
    s.link(r2, c2)
    mesh_i = s.add_mesh([[0, 0, 0]], indices=[0, 0, 0])
    for c in (c1, c2):
        s.nodes[c]["mesh"] = mesh_i
    s.scenes_nodes = [r1, r2]
    doc, _ = s.finish()
    pmap = ev.resolve_parents(doc, ev.Report())
    world = ev.resolve_world_matrices(doc["nodes"], pmap)
    g1 = ev.transform_point(world[c1], [0, 0, 0])
    g2 = ev.transform_point(world[c2], [0, 0, 0])
    check("world/multiple_roots",
          all(abs(g - e) < 1e-6 for g, e in zip(g1, [2, 0, 0]))
          and all(abs(g - e) < 1e-6 for g, e in zip(g2, [0, 6, 0])),
          f"got {g1} / {g2}")


# ------------------------------------------------------ §5 accessor decoding
def run_accessor_cases():
    # handoff §5 case: 2x VEC3, bufferView.byteStride=16, accessor.byteOffset=4
    # layout: [pad4][1,2,3][pad4][4,5,6][pad4]
    buf = (struct.pack("<f", 0.0) + struct.pack("<3f", 1, 2, 3)
           + struct.pack("<f", 0.0) + struct.pack("<3f", 4, 5, 6)
           + struct.pack("<f", 0.0))
    doc = {
        "accessors": [{"bufferView": 0, "byteOffset": 4, "componentType": 5126,
                       "count": 2, "type": "VEC3"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(buf), "byteStride": 16}],
        "buffers": [{"byteLength": len(buf)}],
    }
    got = ev.read_accessor(doc, buf, 0)
    check("accessor/interleaved_stride16_offset4",
          got == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]], f"got {got}")

    buf2 = struct.pack("<6f", 1, 2, 3, 4, 5, 6)
    doc2 = {
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": "VEC3"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 24}],
        "buffers": [{"byteLength": 24}],
    }
    check("accessor/tight_packed",
          ev.read_accessor(doc2, buf2, 0) == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]])

    # normalized ushort weights: 65535 -> 1.0, 32767 -> ~0.5
    buf3 = struct.pack("<4H", 65535, 0, 32767, 0)
    doc3 = {
        "accessors": [{"bufferView": 0, "componentType": 5123, "count": 1,
                       "type": "VEC4", "normalized": True}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        "buffers": [{"byteLength": 8}],
    }
    got3 = ev.read_accessor(doc3, buf3, 0)
    check("accessor/normalized_ushort",
          abs(got3[0][0] - 1.0) < 1e-6 and abs(got3[0][2] - 0.5) < 1e-4, f"got {got3}")

    # MAT4 layout: identity decodes to 16 floats identity
    ident = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    buf4 = struct.pack("<16f", *ident)
    doc4 = {
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1, "type": "MAT4"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 64}],
        "buffers": [{"byteLength": 64}],
    }
    got4 = ev.read_accessor(doc4, buf4, 0)
    check("accessor/mat4_identity", got4[0] == [float(v) for v in ident], f"got {got4}")

    # buffer overrun must raise UnsupportedFeature, not read garbage
    doc5 = {
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 24}],
        "buffers": [{"byteLength": 24}],
    }
    try:
        ev.read_accessor(doc5, buf2, 0)
        check("accessor/bounds_check", False, "overrun read silently")
    except ev.UnsupportedFeature:
        check("accessor/bounds_check", True)


# --------------------------------------- §6/§7/§8 corruption-detection cases
def make_reference_scene():
    """Root with an animated translation, two child meshes: mesh 0 one
    primitive (2 triangles), mesh 1 one primitive (1 triangle)."""
    s = Scene()
    root = s.node(translation=[1, 0, 0])
    n1 = s.node(translation=[0, 2, 0])
    n2 = s.node(translation=[0, 0, 3])
    s.link(root, n1)
    s.link(root, n2)
    s.nodes[n1]["mesh"] = s.add_mesh(
        [[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]],
        indices=[0, 1, 2, 1, 3, 2],
        normals=[[0, 0, 1]] * 4,
    )
    s.nodes[n2]["mesh"] = s.add_mesh(
        [[5, 0, 0], [6, 0, 0], [5, 1, 0]],
        indices=[0, 1, 2],
        normals=[[0, 0, 1]] * 3,
    )
    s.scenes_nodes = [root]
    add_animation(s, root, "translation", [0.0, 1.0], [[0, 0, 0], [1, 0, 0]])
    add_animation(s, root, "rotation", [0.0, 1.0],
                  [[0, 0, 0, 1], [0, 0, 0.7071067811865476, 0.7071067811865476]])
    add_animation(s, root, "scale", [0.0, 1.0], [[1, 1, 1], [1, 1, 1]])
    return s


def run_corruption_cases():
    ref_doc, ref_bin = make_reference_scene().finish()

    def run(name, mutator, expect_codes):
        s = make_reference_scene()
        doc, bin_ = s.finish()
        doc, bin_ = mutator(doc, bin_)
        rep = ev.Report()
        ev.evaluate(doc, bin_, rep, reference=(ref_doc, ref_bin))
        got = [c for c, _ in rep.errors]
        check(name, any(any(g.startswith(e) for e in expect_codes) for g in got), f"{got}")

    # pristine self-comparison must be clean
    rep = ev.Report()
    ev.evaluate(ref_doc, ref_bin, rep, reference=(ref_doc, ref_bin))
    check("corruption/pristine_self_clean", not rep.errors, f"{rep.errors}")

    # §6: vertex count loss (4 -> 3), buffer untouched
    run("corruption/vertex_count_loss",
        lambda d, b: (d, b) if d["accessors"][d["meshes"][0]["primitives"][0]
                       ["attributes"]["POSITION"]].__setitem__("count", 3) is None else None,
        ("REF_VERTEX_COUNT", "REF_PRIMITIVE"))

    # §7: whole material primitive removed
    def drop_prim(d, b):
        d["meshes"][1]["primitives"] = []
        return d, b
    run("corruption/primitive_loss", drop_prim, ("REF_PRIMITIVE_COUNT", "MESH_NO_PRIMITIVES"))

    # §8: last keyframe (1,0,0) -> (99,0,0), channel count unchanged
    def anim_val_99(d, b):
        ch = d["animations"][0]["channels"][0]
        smp = d["animations"][0]["samplers"][ch["sampler"]]
        vacc, bv = d["accessors"][smp["output"]], None
        bv = d["bufferViews"][vacc["bufferView"]]
        off = bv["byteOffset"] + (vacc["count"] - 1) * 12
        ba = bytearray(b)
        struct.pack_into("<f", ba, off, 99.0)
        return d, bytes(ba)
    run("corruption/anim_value_corruption", anim_val_99, ("REF_ANIM_VALUES",))

    # §8b: missing keyframe (layout change, resampled compare must catch it)
    def anim_missing_key(d, b):
        ch = d["animations"][0]["channels"][0]
        smp = d["animations"][0]["samplers"][ch["sampler"]]
        d["accessors"][smp["input"]]["count"] = 1
        d["accessors"][smp["output"]]["count"] = 1
        return d, b
    run("corruption/anim_missing_key", anim_missing_key, ("REF_ANIM_KEYS", "REF_ANIM_DURATION"))

    # §8c: changed LAST-frame quaternion W: 0.707 -> 0.0
    s = make_reference_scene()
    doc, bin_ = s.finish()
    ch = doc["animations"][0]["channels"][1]  # rotation
    smp = doc["animations"][0]["samplers"][ch["sampler"]]
    vacc = doc["accessors"][smp["output"]]
    bv = doc["bufferViews"][vacc["bufferView"]]
    ba = bytearray(bin_)
    struct.pack_into("<f", ba, bv["byteOffset"] + (vacc["count"] - 1) * 16 + 12, 0.0)
    rep = ev.Report()
    ev.evaluate(doc, bytes(ba), rep, reference=(ref_doc, ref_bin))
    check("corruption/anim_quaternion",
          any(c == "REF_ANIM_VALUES" for c, _ in rep.errors), f"{[c for c, _ in rep.errors]}")

    # channel reorder is legal, must NOT false-positive (reversing only the
    # channel list keeps every sampler index reference intact)
    s = make_reference_scene()
    doc, bin_ = s.finish()
    doc["animations"][0]["channels"].reverse()
    rep = ev.Report()
    ev.evaluate(doc, bin_, rep, reference=(ref_doc, ref_bin))
    check("corruption/channel_reorder_ok",
          not any(c.startswith("REF_ANIM") for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")

    # skinned node: weight corruption must be caught (structure + vs reference)
    s = make_reference_scene()
    n3 = s.node(translation=[0, 0, 0])
    joints = s.add_accessor([0, 0, 0, 0], 5123, 4)
    weights = s.add_accessor([1.0, 0.0, 0.0, 0.0], 5126, 4)
    s.nodes[n3]["mesh"] = s.add_mesh([[0, 0, 0], [1, 0, 0], [0, 1, 0]])
    prim = s.meshes[-1]["primitives"][0]
    prim["attributes"]["JOINTS_0"] = joints
    prim["attributes"]["WEIGHTS_0"] = weights
    s.nodes[n3]["skin"] = 0
    ibm = s.add_accessor([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], 5126, 16)
    # patch the skin into the doc after finish() (builder has no skins field)
    doc, bin_ = s.finish()
    doc["skins"] = [{"joints": [n3], "inverseBindMatrices": ibm}]
    ref2_doc, ref2_bin = copy.deepcopy(doc), bytes(bin_)
    # corrupt first weight 1.0 -> 1.5
    wacc = doc["accessors"][weights]
    bv = doc["bufferViews"][wacc["bufferView"]]
    ba = bytearray(bin_)
    struct.pack_into("<f", ba, bv["byteOffset"], 1.5)
    rep = ev.Report()
    ev.evaluate(doc, bytes(ba), rep, reference=(ref2_doc, ref2_bin))
    check("corruption/skin_weight_change",
          any(c in ("SKIN_WEIGHT_SUM", "REF_SKIN_WEIGHTS") for c, _ in rep.errors),
          f"{[c for c, _ in rep.errors]}")


# ------------------------------------------------- T-13 evaluator regressions
def run_t13_cases():
    """bufferView bounds + multi-set skinning witnesses (RED before repair)."""
    s = Scene()
    # CASE 1: accessor data inside BIN but beyond the owning bufferView.
    pos = s.add_accessor([0, 0, 0, 1, 0, 0, 0, 1, 0], 5126, 3)
    bv = s.buffer_views[s.accessors[pos]["bufferView"]]
    bv["byteLength"] = 4
    # keep BIN large enough that only the bufferView limit is violated
    doc, bin_ = s.finish()
    rep = ev.Report()
    try:
        ev.decode_accessor(doc, bin_, pos)
        rejected = False
    except ev.UnsupportedFeature:
        rejected = True
    check("T13/accessor_view_overrun", rejected)

    # CASE 2: valid 8-influence skin split across JOINTS_0/1, WEIGHTS_0/1.
    s2 = Scene()
    n0 = s2.node(name="root", translation=[0, 0, 0], rotation=[0, 0, 0, 1], scale=[1, 1, 1])
    pos2 = s2.add_accessor([0, 0, 0, 1, 0, 0, 0, 1, 0], 5126, 3)
    s2.meshes.append({"primitives": [{"attributes": {"POSITION": pos2}, "mode": 4}]})
    s2.nodes[0]["mesh"] = 0
    joints = list(range(8))
    j0 = s2.add_accessor([j for j in joints[:4] for _ in range(3)], 5123, 4)
    j1 = s2.add_accessor([j for j in joints[4:] for _ in range(3)], 5123, 4)
    w0 = s2.add_accessor([0.125] * 12, 5126, 4)
    w1 = s2.add_accessor([0.125] * 12, 5126, 4)
    s2.meshes[0]["primitives"][0]["attributes"].update(
        {"JOINTS_0": j0, "JOINTS_1": j1, "WEIGHTS_0": w0, "WEIGHTS_1": w1})
    s2.nodes[0]["skin"] = 0
    s2.scenes_nodes = [0]
    s2.animations = []
    ibm = s2.add_accessor([float(x) for _ in range(8)
                            for x in (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)], 5126, 16)
    doc2, bin2 = s2.finish()
    doc2["skins"] = [{"joints": joints, "inverseBindMatrices": ibm}]
    rep2 = ev.Report()
    ev.evaluate(doc2, bin2, rep2)
    check("T13/multiset_skin_accepted",
          not any(c in ("SKIN_WEIGHT_SUM", "SKIN_SET_MISMATCH", "SKIN_COUNT_MISMATCH") for c, _ in rep2.errors),
          f"{[c for c, _ in rep2.errors]}")

    # CASE 3: extra weight set corrupted (0.125 -> 1.0) must be detected.
    doc3, bin3 = copy.deepcopy(doc2), bytearray(bin2)
    acc3 = doc3["accessors"][w1]
    bv3 = doc3["bufferViews"][acc3["bufferView"]]
    struct.pack_into("<f", bin3, bv3["byteOffset"], 1.0)
    rep3 = ev.Report()
    ev.evaluate(doc3, bytes(bin3), rep3)
    check("T13/multiset_skin_extra_corruption_detected",
          any(c == "SKIN_WEIGHT_SUM" for c, _ in rep3.errors),
          f"{[c for c, _ in rep3.errors]}")


# ----------------------------------------------------------------------- main
def main():
    run_world_matrix_cases()
    run_accessor_cases()
    run_corruption_cases()
    run_t13_cases()
    print()
    if FAILURES:
        print(f"ORACLE RESULT: {len(FAILURES)} FAILED: {FAILURES}")
        return 1
    print("ORACLE RESULT: ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
