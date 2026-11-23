"""Adversarial matrix for the independent glTF evaluator (SAIHANDOFF-12 §9).

Each case corrupts a copy of a known-good exported GLB. The evaluator must
FAIL every corrupted variant and PASS the pristine baseline. Contrary to the
T-9 revision, every case is FEATURE-AWARE: when the baseline lacks the
structure a mutation targets, the case reports SKIP_NO_FIXTURE instead of
crashing (KeyError) or silently passing. Coverage gates downstream must not
accept skip-substitution for mandatory release cases.

Run: python tools/adversarial_matrix_T9.py <baseline.glb>
Exit 0 = matrix satisfied, 1 = evaluator missed at least one corruption
(or crashed), 2 = baseline itself invalid.
"""
import json
import struct
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gltf_scene_evaluator as ev


def load(path):
    return ev.read_glb(path)


def pack_glb(doc, binchunk):
    js = json.dumps(doc, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    bin_ = bytearray(binchunk)
    while len(bin_) % 4:
        bin_.append(0)
    total = 12 + 8 + len(js) + 8 + len(bin_)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(bin_), 0x004E4942) + bytes(bin_)
    return bytes(out)


def _first_accessor_view(doc, kind):
    """(accessor, bufferView) of the first primitive attribute 'kind', or None."""
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            idx = prim.get("attributes", {}).get(kind)
            if idx is None:
                continue
            acc = doc["accessors"][idx]
            return acc, doc["bufferViews"][acc["bufferView"]]
    return None


def first_prim(doc, mesh_i=0, prim_i=0):
    return doc["meshes"][mesh_i]["primitives"][prim_i], doc["meshes"][mesh_i]["primitives"][prim_i]["attributes"]


def has_embedding(doc, magic):
    for img in doc.get("images", []):
        if "bufferView" not in img:
            continue
        bv = doc["bufferViews"][img["bufferView"]]
        blob = bytes(doc.get("_bin", b"")[bv.get("byteOffset", 0):bv.get("byteOffset", 0) + 8])
        if blob.startswith(magic):
            return True
    return False


# ------------------------------------------------------------- mutations
def corrupt_normals(doc, bin_):
    """1. Reversed world normals: negate every NORMAL component."""
    found = _first_accessor_view(doc, "NORMAL")
    if not found:
        return None
    acc, bv = found
    base, n = bv["byteOffset"], acc["count"] * 3
    bin_ = bytearray(bin_)
    for k in range(n):
        off = bv["byteOffset"] + k * 4
        v = struct.unpack_from("<f", bin_, off)[0]
        struct.pack_into("<f", bin_, off, -v)
    return doc, bytes(bin_)


def drop_joint_attrs(doc, bin_):
    """2. Missing joint attributes while node.skin stays."""
    holders = [i for i, n in enumerate(doc["nodes"]) if "skin" in n]
    if not holders:
        return None
    touched = False
    for n in (doc["nodes"][i] for i in holders):
        if "mesh" not in n:
            continue
        for prim in doc["meshes"][n["mesh"]]["primitives"]:
            attrs = prim["attributes"]
            if any(k.startswith(("JOINTS_", "WEIGHTS_")) for k in attrs):
                touched = True
                for k in list(attrs):
                    if k.startswith("JOINTS_") or k.startswith("WEIGHTS_"):
                        del attrs[k]
    if not touched:
        return None
    return doc, bin_


def drop_material(doc, bin_):
    """3. Missing material binding: remove references and definitions."""
    touched = False
    for mesh in doc.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if "material" in prim:
                prim.pop("material", None)
                touched = True
    if not touched and not doc.get("materials"):
        return None
    if doc.get("materials"):
        doc["materials"] = []
        touched = True
    if not touched:
        return None
    return doc, bin_


def corrupt_embedded_jpeg(doc, bin_):
    """4. Corrupt JPEG: smash the embedded image bytes."""
    for img in doc.get("images", []):
        if "bufferView" not in img:
            continue
        bv = doc["bufferViews"][img["bufferView"]]
        blob = bytes(bin_[bv["byteOffset"]:bv["byteOffset"] + 2])
        if blob[:2] == b"\xff\xd8":
            bin_ = bytearray(bin_)
            for k in range(bv["byteLength"]):
                bin_[bv["byteOffset"] + k] = 0x7F
            return doc, bytes(bin_)
    return None


def corrupt_png_header(doc, bin_):
    """5. Corrupt PNG: zero the signature."""
    PNG_SIG = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
    for img in doc.get("images", []):
        if "bufferView" not in img:
            continue
        bv = doc["bufferViews"][img["bufferView"]]
        blob = bytes(bin_[bv["byteOffset"]:bv["byteOffset"] + 8])
        if blob == PNG_SIG:
            bin_ = bytearray(bin_)
            for k in range(8):
                bin_[bv["byteOffset"] + k] = 0
            return doc, bytes(bin_)
    return None


def inject_cycle(doc, bin_):
    """6. Node cycle between two existing nodes."""
    n = len(doc["nodes"])
    if n < 2:
        return None
    a, b = 0, 1
    for i, node in enumerate(doc["nodes"]):
        if node.get("children"):
            a = i
            b = node["children"][0]
            break
    if a == b:
        return None
    if b in doc["nodes"][a].get("children", []) and a in doc["nodes"][b].get("children", []):
        return None  # already a cycle
    doc["nodes"][a].setdefault("children", []).append(b)
    doc["nodes"][b].setdefault("children", []).append(a)
    return doc, bin_


def corrupt_instance_transform(doc, bin_):
    """7. Wrong instanced transform: shift first mesh node by 100 units."""
    nodes = doc["nodes"]
    holders = [i for i, n in enumerate(nodes) if "mesh" in n]
    if not holders:
        return None
    i = holders[0]
    nodes[i]["translation"] = [x + 100.0 for x in nodes[i].get("translation", [0, 0, 0])]
    return doc, bin_


def swap_uv(doc, bin_):
    """8. Swapped UV: exchange U and V per vertex."""
    found = _first_accessor_view(doc, "TEXCOORD_0")
    if not found:
        return None
    acc, bv = found
    bin_ = bytearray(bin_)
    base = bv["byteOffset"] + 0
    for k in range(acc["count"]):
        off = bv["byteOffset"] + k * 8  # exporter writes tightly packed VEC2
        u, v = struct.unpack_from("<ff", bin_, off)
        struct.pack_into("<ff", bin_, off, v, u)
    return doc, bytes(bin_)


def corrupt_skin_weights(doc, bin_):
    """9. First vertex loses weight mass."""
    found = _first_accessor_view(doc, "WEIGHTS_0")
    if not found:
        return None
    acc, bv = found
    bin_ = bytearray(bin_)
    comp = 4 if acc["type"] == "VEC4" else 3
    fmt = {4: "<ffff", 3: "<fff"}[acc["type"] and 4] if acc["type"] == "VEC4" else "<fff"
    vals = [1.5, 0.0, 0.0, 0.0][:4] if acc["type"] == "VEC4" else [1.5, 0.0, 0.0]
    struct.pack_into("<ffff" if acc["type"] == "VEC4" else "<fff", bin_, bv["byteOffset"], *vals)
    return doc, bytes(bin_)


def corrupt_inverse_binds(doc, bin_):
    """10. Zero out the first inverse bind matrix."""
    for skin in doc.get("skins", []):
        if "inverseBindMatrices" not in skin:
            continue
        acc = doc["accessors"][skin["inverseBindMatrices"]]
        bv = doc["bufferViews"][acc["bufferView"]]
        bin_ = bytearray(bin_)
        for k in range(16):
            struct.pack_into("<f", bin_, bv["byteOffset"] + k * 4, 0.0)
        return doc, bytes(bin_)
    return None


def drop_animation_track(doc, bin_):
    """11. Empty an animation's channels."""
    if not doc.get("animations"):
        return None
    if not doc["animations"][0].get("channels"):
        return None
    doc["animations"][0]["channels"] = []
    return doc, bin_


def corrupt_keyframes(doc, bin_):
    """12. Unsorted sampler times."""
    if not doc.get("animations"):
        return None
    smp = doc["animations"][0]["samplers"][0]
    acc = doc["accessors"][smp["input"]]
    bv = doc["bufferViews"][acc["bufferView"]]
    n = acc["count"]
    if n < 2:
        return None
    bin_ = bytearray(bin_)
    for k in range(n):
        struct.pack_into("<f", bin_, bv["byteOffset"] + k * 4, float(n - k))
    return doc, bytes(bin_)


def drop_primitive_geometry(doc, bin_):
    """13. Vertex positions zeroed (geometry destroyed, counts intact)."""
    found = _first_accessor_view(doc, "POSITION")
    if not found:
        return None
    acc, bv = found
    bin_ = bytearray(bin_)
    for k in range(acc["count"] * 3):
        struct.pack_into("<f", bin_, bv["byteOffset"] + k * 4, 0.0)
    return doc, bytes(bin_)


def shift_uv(doc, bin_):
    """14. UV shift by +0.5 (layout corruption inside tolerance range)."""
    found = _first_accessor_view(doc, "TEXCOORD_0")
    if not found:
        return None
    acc, bv = found
    bin_ = bytearray(bin_)
    for k in range(acc["count"]):
        off = bv["byteOffset"] + k * 8
        u, v = struct.unpack_from("<ff", bin_, off)
        struct.pack_into("<ff", bin_, off, u + 0.5, v)
    return doc, bytes(bin_)


CASES = [
    ("reversed_world_normals", corrupt_normals),
    ("missing_joint_attributes", drop_joint_attrs),
    ("missing_material", drop_material),
    ("corrupt_jpeg", corrupt_embedded_jpeg),
    ("corrupt_png", corrupt_png_header),
    ("node_cycle", inject_cycle),
    ("instance_transform_offset", corrupt_instance_transform),
    ("swapped_uv", swap_uv),
    ("changed_skin_weights", corrupt_skin_weights),
    ("invalid_inverse_binds", corrupt_inverse_binds),
    ("missing_animation_track", drop_animation_track),
    ("wrong_keyframes", corrupt_keyframes),
    ("zeroed_geometry", drop_primitive_geometry),
    ("shifted_uv", shift_uv),
]

MANDATORY_RELEASE_CASES = {
    "reversed_world_normals",
    "zeroed_geometry",
    "instance_transform_offset",
    "changed_skin_weights",
    "invalid_inverse_binds",
    "missing_animation_track",
}


def run_matrix(baseline_path, json_out=None):
    doc0, bin0 = load(baseline_path)

    # baseline must PASS structurally AND against itself (reference compare)
    rep = ev.Report()
    ev.evaluate(json.loads(json.dumps(doc0)), bytes(bin0), rep)
    structural_ok = not rep.errors
    rep = ev.Report()
    ev.evaluate(json.loads(json.dumps(doc0)), bytes(bin0), rep,
                reference=(json.loads(json.dumps(doc0)), bytes(bin0)))
    base_ok = structural_ok and not rep.errors
    print(f"baseline: {'PASS' if base_ok else 'FAIL'}")
    if not base_ok:
        print("MATRIX FAILED: baseline itself does not pass")
        return 2

    results = {}
    ref = (json.loads(json.dumps(doc0)), bytes(bin0))
    for name, fn in CASES:
        doc = json.loads(json.dumps(doc0))
        try:
            out = fn(doc, bytes(bin0))
        except Exception as e:
            results[name] = f"CRASH({type(e).__name__}: {e})"
            print(f"{name}: CRASH {type(e).__name__}: {e}")
            continue
        if out is None:
            results[name] = "SKIP_NO_FIXTURE"
            print(f"{name}: SKIP_NO_FIXTURE")
            continue
        d, b = out
        rep = ev.Report()
        try:
            ev.evaluate(d, b, rep, reference=ref)
        except Exception as e:
            results[name] = f"CRASH({type(e).__name__}: {e})"
            print(f"{name}: CRASH {type(e).__name__}: {e}")
            continue
        if rep.errors:
            results[name] = "DETECTED"
            print(f"{name}: DETECTED ({rep.errors[0][0]})")
        else:
            results[name] = "MISSED"
            print(f"{name}: !!! MISSED — corruption passed validation")

    missed = sorted(k for k, v in results.items() if v == "MISSED")
    crashed = sorted(k for k, v in results.items() if v.startswith("CRASH"))
    mandatory_missing = sorted(MANDATORY_RELEASE_CASES - set(results))
    skipped = sorted(k for k, v in results.items() if v == "SKIP_NO_FIXTURE")

    if json_out:
        import json as _json
        with open(json_out, "w") as f:
            _json.dump(
                {
                    "baseline": str(baseline_path),
                    "baseline_pass": base_ok,
                    "results": results,
                    "skipped_no_fixture": skipped,
                    "missed": missed,
                    "crashed": crashed,
                    "mandatory_cases_not_present": mandatory_missing,
                },
                f,
                indent=2,
            )

    if crashed:
        print(f"MATRIX FAILED: {len(crashed)} case(s) crashed: {crashed}")
        return 1
    if missed:
        print(f"MATRIX FAILED: {len(missed)} corruption(s) undetected: {missed}")
        return 1
    print(f"MATRIX RESULT: all present corruptions detected "
          f"({len(results) - len(skipped)} executed, {len(skipped)} skip-no-fixture)")
    if skipped:
        print("WARNING: skip-no-fixture cases present; mandatory release gates must not count them as coverage")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    out = None
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
    sys.exit(run_matrix(sys.argv[1], json_out=out))
