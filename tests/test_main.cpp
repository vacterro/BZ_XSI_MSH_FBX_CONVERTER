#include "bzconverter/core/types.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/core/output_bundle.hpp"
#include "bzconverter/formats/probe.hpp"
#include "bzconverter/formats/xsi/xsi_parser.hpp"
#include "bzconverter/materials/material_resolver.hpp"
#include "bzconverter/scene/scene.hpp"
#include "bzconverter/exporters/exporter.hpp"
#include "bzconverter/exporters/fbx_exporter.hpp"
#include "bzconverter/exporters/glb_exporter.hpp"
#include "bzconverter/exporters/obj_exporter.hpp"
#include <cmath>
#include "bzconverter/validation/validator.hpp"

#include <iostream>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <string>
#include "test_main_t9.cpp"
#include "test_main_t13.cpp"

namespace fs = std::filesystem;

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            return false; \
        } \
    } while(0)

#define RUN_TEST(fn) \
    do { \
        g_tests_run++; \
        std::cout << "[RUN]  " << #fn << "..." << std::flush; \
        if (fn()) { \
            g_tests_passed++; \
            std::cout << " PASSED\n"; \
        } else { \
            g_tests_failed++; \
        } \
    } while(0)

static fs::path find_sample_xsi() {
    std::vector<fs::path> candidates = {
        "xsi's/ivtank00.xsi",
        "../xsi's/ivtank00.xsi",
        "../../xsi's/ivtank00.xsi",
        "xsi's/sgbzka_stand.xsi",
        "../xsi's/sgbzka_stand.xsi"
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return {};
}

bool test_probe_valid_xsi() {
    fs::path p = find_sample_xsi();
    TEST_ASSERT(!p.empty(), "Could not find sample .xsi file for probe test");

    bz::ProbeResult pr = bz::probe_file(p);
    TEST_ASSERT(pr.supported, "Probe should mark sample .xsi as supported");
    TEST_ASSERT(pr.format == bz::SourceFormat::XSI_Text_11 || pr.format == bz::SourceFormat::XSI_Text_13,
                "Format should be XSI_Text_11 or XSI_Text_13");
    TEST_ASSERT(pr.file_size > 0, "File size must be positive");
    TEST_ASSERT(!pr.sha256.empty(), "SHA256 must be computed");
    return true;
}

bool test_probe_invalid_file() {
    std::vector<uint8_t> dummy = {'R', 'A', 'N', 'D', 'O', 'M', 0, 1, 2, 3};
    bz::ProbeResult pr = bz::probe_bytes(dummy, "dummy.bin");
    TEST_ASSERT(!pr.supported, "Random bytes should not be supported");
    TEST_ASSERT(pr.format == bz::SourceFormat::Unknown, "Format should be Unknown");
    return true;
}

bool test_parse_xsi_geometry() {
    fs::path p = find_sample_xsi();
    TEST_ASSERT(!p.empty(), "Sample .xsi file not found");

    bz::Scene scene;
    bz::ProbeResult probe;
    std::string err;
    bool ok = bz::load_model_file(p, scene, probe, err, true);
    TEST_ASSERT(ok, ("Parsing failed: " + err).c_str());
    TEST_ASSERT(!scene.nodes.empty(), "Scene should contain nodes");
    TEST_ASSERT(!scene.meshes.empty(), "Scene should contain meshes");

    auto st = scene.compute_stats();
    TEST_ASSERT(st.total_triangles > 0, "Scene must have triangles");
    TEST_ASSERT(st.total_vertices > 0, "Scene must have vertices");
    TEST_ASSERT(st.node_count > 0, "Node count must be > 0");
    return true;
}

bool test_skin_influence_cap() {
    // 5 bones; vertex has 6 influences sorted desc, one on an unmapped node (99).
    std::unordered_map<int, uint16_t> n2j = {{10,0},{11,1},{12,2},{13,3},{14,4}};
    std::vector<bz::BoneWeight> wl = {{10,0.30f},{99,0.20f},{11,0.15f},{12,0.15f},{13,0.10f},{14,0.10f}};
    uint16_t j[4]; float w[4];
    bz::pack_skin_influences(wl, n2j, j, w);
    float sum = w[0] + w[1] + w[2] + w[3];
    TEST_ASSERT(std::fabs(sum - 1.0f) < 1e-5f, "WEIGHTS_0 must sum to 1 after capping");
    for (int s = 0; s < 4; ++s) TEST_ASSERT(w[s] > 0.0f, "All 4 slots must hold mapped influences");
    TEST_ASSERT(j[0] == 0 && j[1] == 1 && j[2] == 2 && j[3] == 3, "Strongest mapped joints kept in order");
    TEST_ASSERT(std::fabs(w[0] - 0.30f / 0.70f) < 1e-5f, "Renormalized weight value");
    // No usable influence -> joint 0 weight 1
    std::vector<bz::BoneWeight> none = {{99,0.5f}};
    bz::pack_skin_influences(none, n2j, j, w);
    for (int s = 0; s < 4; ++s)
        TEST_ASSERT(w[s] == 0.0f, "Unmapped-only vertex must keep zero weights, never artificial bone assignment");
    return true;
}
bool test_export_glb() {
    fs::path p = find_sample_xsi();
    TEST_ASSERT(!p.empty(), "Sample .xsi file not found");

    fs::path out_glb = fs::temp_directory_path() / "bzconv_test_output.glb";
    std::error_code ec;
    fs::remove(out_glb, ec);

    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false; // Fast pure geometry GLB test
    opts.include_textures = false;
    opts.overwrite = true;

    bz::MaterialResolver resolver;
    bz::ExportResult res = bz::convert_single_file(p, out_glb, opts, resolver);

    TEST_ASSERT(res.status == bz::JobStatus::Success || res.status == bz::JobStatus::Partial,
                ("GLB conversion failed: " + res.summary).c_str());
    TEST_ASSERT(fs::exists(out_glb), "GLB output file must exist on disk");

    std::vector<uint8_t> glb_bytes;
    std::string read_err;
    bool read_ok = bz::read_file_bytes(out_glb, glb_bytes, read_err);
    TEST_ASSERT(read_ok, "Failed to read back GLB file");
    TEST_ASSERT(glb_bytes.size() >= 20, "GLB header size too small");

    // Check glTF binary header magic: 0x46546C67 ("glTF")
    uint32_t magic = *reinterpret_cast<const uint32_t*>(glb_bytes.data());
    uint32_t version = *reinterpret_cast<const uint32_t*>(glb_bytes.data() + 4);
    TEST_ASSERT(magic == 0x46546C67, "GLB magic mismatch");
    TEST_ASSERT(version == 2, "GLB version must be 2");

    fs::remove(out_glb, ec);
    return true;
}

bool test_export_obj() {
    fs::path p = find_sample_xsi();
    TEST_ASSERT(!p.empty(), "Sample .xsi file not found");

    fs::path out_obj = fs::temp_directory_path() / "bzconv_test_output.obj";
    std::error_code ec;
    fs::remove(out_obj, ec);

    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::OBJ;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;

    bz::MaterialResolver resolver;
    bz::ExportResult res = bz::convert_single_file(p, out_obj, opts, resolver);

    TEST_ASSERT(res.status == bz::JobStatus::Success || res.status == bz::JobStatus::Partial,
                ("OBJ conversion failed: " + res.summary).c_str());
    TEST_ASSERT(fs::exists(out_obj), "OBJ output file must exist");

    std::vector<uint8_t> obj_bytes;
    std::string read_err;
    bz::read_file_bytes(out_obj, obj_bytes, read_err);
    std::string obj_str(obj_bytes.begin(), obj_bytes.end());
    TEST_ASSERT(obj_str.find("v ") != std::string::npos, "OBJ must contain geometric vertices 'v '");
    TEST_ASSERT(obj_str.find("f ") != std::string::npos, "OBJ must contain face elements 'f '");

    fs::remove(out_obj, ec);
    return true;
}

bool test_export_fbx_if_blender_available() {
    auto binfo = bz::detect_blender();
    if (!binfo.found) {
        std::cout << " (SKIPPED: Blender not detected) ";
        return true;
    }

    fs::path p = find_sample_xsi();
    TEST_ASSERT(!p.empty(), "Sample .xsi file not found");

    fs::path out_fbx = fs::temp_directory_path() / "bzconv_test_output.fbx";
    std::error_code ec;
    fs::remove(out_fbx, ec);

    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::FBX;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.include_animations = false;
    opts.validate_output = true;
    opts.overwrite = true;

    bz::MaterialResolver resolver;
    bz::ExportResult res = bz::convert_single_file(p, out_fbx, opts, resolver);

    TEST_ASSERT(res.status == bz::JobStatus::Success || res.status == bz::JobStatus::Partial,
                ("FBX conversion failed: " + res.summary).c_str());
    TEST_ASSERT(fs::exists(out_fbx), "FBX output file must exist");
    // ivtank00.xsi FBX round-trip has a documented FBX bone-parent representation
    // difference (hardpoint/bone aliases); never claim GREEN when strict check fails.
    if (!res.validation.passed) {
        bool only_hierarchy = !res.validation.issues.empty();
        for (const auto& issue : res.validation.issues)
            if (issue.find("Hierarchy parent links changed") == std::string::npos) only_hierarchy = false;
        TEST_ASSERT(only_hierarchy, ("Unexpected FBX fidelity failures: " + res.summary).c_str());
        TEST_ASSERT(res.status == bz::JobStatus::Partial, "Hierarchy representation difference must be PARTIAL, never SUCCESS");
    }

    fs::remove(out_fbx, ec);
    return true;
}

bool test_animation_only_xsi() {
    std::vector<fs::path> candidates = {
        "xsi's/mebridge2.xsi",
        "../xsi's/mebridge2.xsi",
        "../../xsi's/mebridge2.xsi"
    };
    fs::path p;
    for (const auto& c : candidates) {
        if (fs::exists(c)) { p = c; break; }
    }
    TEST_ASSERT(!p.empty(), "mebridge2.xsi not found");

    bz::Scene scene;
    bz::ProbeResult probe;
    std::string err;
    bool ok = bz::load_model_file(p, scene, probe, err, true);
    TEST_ASSERT(ok, ("Failed to parse animation-only XSI: " + err).c_str());
    TEST_ASSERT(scene.is_animation_only, "Scene should be flagged as animation-only");
    TEST_ASSERT(!scene.animations.empty(), "Scene should contain animation clips");
    TEST_ASSERT(scene.animations[0].tracks.size() == 12, "Should parse 12 animation tracks");
    return true;
}

bool test_real_docb_msh_if_available() {
    fs::path msh_path = "P:/__SAVE_P/___VAC/_BATTLEZONE/_BZCC/_RAW/bz2r_res/baked/ISDF/vehicles/ivtank00.msh";
    if (!fs::exists(msh_path)) {
        std::cout << " (SKIPPED: P: drive fixture not found) ";
        return true;
    }

    bz::ProbeResult pr = bz::probe_file(msh_path);
    TEST_ASSERT(pr.supported, "ivtank00.msh must be recognized as supported");
    TEST_ASSERT(pr.format == bz::SourceFormat::MSH_DOCB_V1_BZCC, "Format must be MSH_DOCB_V1_BZCC");

    bz::Scene scene;
    std::string err;
    bool ok = bz::load_model_file(msh_path, scene, pr, err, true);
    TEST_ASSERT(ok, ("Failed to parse ivtank00.msh: " + err).c_str());
    auto st = scene.compute_stats();
    TEST_ASSERT(st.node_count == 30, "ivtank00.msh should have 30 nodes");
    TEST_ASSERT(st.total_triangles == 1110, "ivtank00.msh should have 1110 triangles");
    TEST_ASSERT(scene.animations.size() == 3, "ivtank00.msh should have 3 animation clips");

    fs::path out_glb = fs::temp_directory_path() / "bzconv_ivtank_msh_test.glb";
    std::error_code ec;
    fs::remove(out_glb, ec);

    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;

    bz::MaterialResolver resolver;
    bz::ExportResult res = bz::convert_single_file(msh_path, out_glb, opts, resolver);
    TEST_ASSERT(res.status == bz::JobStatus::Success, ("MSH GLB conversion failed: " + res.summary).c_str());
    TEST_ASSERT(fs::exists(out_glb), "GLB output file must exist");
    fs::remove(out_glb, ec);
    return true;
}

bool test_atomic_write_file_safety() {
    fs::path test_p = fs::temp_directory_path() / "bzconv_atomic_test.bin";
    std::error_code ec;
    fs::remove(test_p, ec);

    std::string initial_data = "INITIAL_VALID_DATA";
    std::string err;
    bool ok = bz::write_file_atomic(test_p, initial_data.data(), initial_data.size(), err);
    TEST_ASSERT(ok, ("Initial atomic write failed: " + err).c_str());
    TEST_ASSERT(fs::exists(test_p), "Test file must exist");

    std::string new_data = "NEW_REPLACED_DATA";
    ok = bz::write_file_atomic(test_p, new_data.data(), new_data.size(), err);
    TEST_ASSERT(ok, ("Secondary atomic replacement failed: " + err).c_str());

    std::vector<uint8_t> bytes;
    bz::read_file_bytes(test_p, bytes, err);
    std::string read_str(bytes.begin(), bytes.end());
    TEST_ASSERT(read_str == new_data, "Content should match replaced data");

    fs::remove(test_p, ec);
    return true;
}

bool test_skin_payload_preserves_8_influences() {
    bz::Scene scene;
    scene.source_name = "eight_influences";
    for (int i = 0; i < 8; ++i) {
        bz::Node n; n.name = "bone" + std::to_string(i); n.is_bone = true;
        scene.nodes.push_back(n);
    }
    bz::Mesh m;
    m.name = "skinned";
    m.is_skinned = true;
    m.vertices.resize(1);
    m.vertices[0].pos = {0, 0, 0};
    m.vertices[0].normal = {0, 1, 0};
    m.vertices[0].uv = {0.25f, 0.75f};
    m.indices = {0, 0, 0}; // degenerate but sufficient for payload test
    std::vector<bz::BoneWeight> eight;
    for (int i = 0; i < 8; ++i) eight.push_back({i, 1.0f / 8.0f});
    m.skin_weights.push_back(eight);
    scene.meshes.push_back(m);
    scene.normalize_skin_weights();

    bz::ExportOptions opts;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;

    bz::MaterialResolver resolver;
    fs::path out = fs::temp_directory_path() / "bzconv_eight_influences.glb";
    std::error_code ec;
    fs::remove(out, ec);
    // Conversion needs a source on disk; write a minimal payload via GLB exporter directly.
    std::vector<bz::Diagnostic> diags;
    std::string err;
    bool ok = bz::export_scene_gltf(scene, out, true, opts, diags, err);
    TEST_ASSERT(ok, ("8-influence GLB export failed: " + err).c_str());
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "Cannot read exported GLB");
    TEST_ASSERT(bytes.size() > 100, "Exported GLB too small");
    fs::remove(out, ec);
    TEST_ASSERT(scene.meshes[0].skin_weights[0].size() == 8, ("Scene must keep all 8 influences, got " + std::to_string(scene.meshes[0].skin_weights[0].size())).c_str());
    float sum = 0.0f;
    for (const auto& bw : scene.meshes[0].skin_weights[0]) sum += bw.weight;
    TEST_ASSERT(std::fabs(sum - 1.0f) < 1e-5f, "Influences must be normalized");

    // Independent GLB binary check: two JOINTS/WEIGHTS sets must exist.
    bz::Json gltf;
    std::vector<uint8_t> bin_chunk;
    auto read_u32 = [&](size_t off) {
        return uint32_t(bytes[off]) | (uint32_t(bytes[off+1]) << 8) |
               (uint32_t(bytes[off+2]) << 16) | (uint32_t(bytes[off+3]) << 24);
    };
    TEST_ASSERT(bytes.size() > 20 && read_u32(0) == 0x46546C67, "GLB header");
    size_t off = 12;
    while (off + 8 <= bytes.size()) {
        uint32_t len = read_u32(off), type = read_u32(off + 4);
        if (type == 0x4E4F534A) {
            std::string js(reinterpret_cast<const char*>(bytes.data() + off + 8), len);
            std::string jerr;
            TEST_ASSERT(bz::Json::parse(js, gltf, jerr), ("GLB JSON parse: " + jerr).c_str());
        } else if (type == 0x004E4942) {
            bin_chunk.assign(bytes.begin() + off + 8, bytes.begin() + off + 8 + len);
        }
        off += 8 + len;
    }
    const auto* meshes = gltf.get("meshes");
    TEST_ASSERT(meshes && !meshes->as_arr().empty(), "GLB must contain meshes");
    const auto& prims = (*meshes->as_arr()[0].get("primitives")).as_arr();
    TEST_ASSERT(!prims.empty(), "GLB mesh must contain primitives");
    const auto* attrs = prims[0].get("attributes");
    TEST_ASSERT(attrs && attrs->get("JOINTS_0") && attrs->get("JOINTS_1"), "GLB must expose JOINTS_0 and JOINTS_1 for 8 influences");
    TEST_ASSERT(attrs->get("WEIGHTS_0") && attrs->get("WEIGHTS_1"), "GLB must expose WEIGHTS_0 and WEIGHTS_1 for 8 influences");
    return true;
}

bool test_overwrite_policy() {
    fs::path p = fs::temp_directory_path() / "bzconv_overwrite_policy.bin";
    std::error_code ec;
    fs::remove(p, ec);
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(p, "ORIGINAL", 8, err), ("Initial write: " + err).c_str());
    bz::ExportOptions opts;
    opts.overwrite = false;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false;
    opts.include_textures = false;
    bz::MaterialResolver resolver;
    bz::ExportResult r = bz::convert_single_file(find_sample_xsi(), p, opts, resolver);
    TEST_ASSERT(r.status == bz::JobStatus::Failed, "Existing output must fail without overwrite");
    TEST_ASSERT(r.result_class == bz::ResultClass::Failed, "Class must be FAILED");
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(p, bytes, err), "Original vanished");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == "ORIGINAL", "Original bytes must be untouched");
    fs::remove(p, ec);
    return true;
}

bool test_fbx_validation_no_false_pass() {
    // A validation result with issues must never be reported as passed.
    bz::ValidationMetrics v;
    v.ran = true;
    v.passed = true;
    bz::adjudicate_fbx_validation(v);
    TEST_ASSERT(v.passed, "Clean validation must pass");
    v.issues.push_back("Low normal/winding agreement after FBX re-import (40%)");
    v.normals_ok = false;
    v.passed = true; // simulate stale cached pass
    bz::adjudicate_fbx_validation(v);
    TEST_ASSERT(!v.passed, "Validation with issue must not pass");
    v.issues.clear();
    v.normals_ok = false;
    v.passed = true;
    bz::adjudicate_fbx_validation(v);
    TEST_ASSERT(!v.passed, "Any failed feature must not pass");
    return true;
}

bool test_result_classification() {
    bz::ExportResult r;
    r.status = bz::JobStatus::Success;
    r.result_class = bz::ResultClass::VerifiedSuccess;
    TEST_ASSERT(std::string(bz::result_class_to_string(r.result_class)) == "VERIFIED_SUCCESS", "Class string");
    r.result_class = bz::ResultClass::NeedsCompanionAsset;
    TEST_ASSERT(std::string(bz::result_class_to_string(r.result_class)) == "NEEDS_COMPANION_ASSET", "Companion class");
    return true;
}

bool test_companion_node_index_remap() {
    bz::Scene anim, base;
    auto mk = [](const char* n) { bz::Node x; x.name = n; return x; };
    anim.nodes.push_back(mk("turret"));   // index 0
    anim.nodes.push_back(mk("barrel"));   // index 1
    bz::AnimationClip clip; clip.name = "walk";
    bz::AnimTrack t0; t0.node_index = 0; t0.node_name = "turret";
    bz::AnimTrack t1; t1.node_index = 1; t1.node_name = "barrel";
    clip.tracks = {t0, t1}; anim.animations.push_back(clip);
    base.nodes.push_back(mk("body"));     // index 0
    base.nodes.push_back(mk("barrel"));   // index 1
    base.nodes.push_back(mk("turret"));   // index 2
    base.meshes.resize(1); base.meshes[0].name = "body_mesh";
    std::unordered_map<int,int> map;
    std::string reason;
    TEST_ASSERT(bz::build_companion_node_map(anim, base, map, reason), ("remap failed: " + reason).c_str());
    TEST_ASSERT(map[0] == 2, "turret must map to companion node 2, not 0");
    TEST_ASSERT(map[1] == 1, "barrel must map to companion node 1");
    // Ambiguous nodes without hierarchy/bind evidence must be refused.
    bz::Scene amb = base; amb.nodes.push_back(mk("turret"));
    map.clear(); reason.clear();
    TEST_ASSERT(!bz::build_companion_node_map(anim, amb, map, reason), "ambiguous match must fail");
    return true;
}

bool test_animation_only_auto_needs_companion() {
    std::vector<fs::path> candidates = {"xsi's/mebridge2.xsi", "../xsi's/mebridge2.xsi"};
    fs::path p;
    for (const auto& c : candidates) if (fs::exists(c)) { p = c; break; }
    if (p.empty()) { std::cout << " (SKIPPED: animation-only fixture) "; return true; }
    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.companion_model = fs::path("NO_SUCH_COMPANION_WILL_NOT_MATCH.xsi");
    bz::MaterialResolver resolver;
    fs::path out = fs::temp_directory_path() / "bzconv_anim_only_companion.glb";
    std::error_code ec;
    fs::remove(out, ec);
    bz::ExportResult r = bz::convert_single_file(p, out, opts, resolver);
    TEST_ASSERT(r.result_class == bz::ResultClass::NeedsCompanionAsset,
                ("Animation-only must need companion, got: " + r.summary).c_str());
    TEST_ASSERT(!fs::exists(out, ec), "No output should be produced without companion");
    return true;
}

bool test_source_level_p0_guards() {
    auto read_src = [](const char* rel) {
        std::ifstream in(rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    std::string gui = read_src("src/gui/gui_main.cpp");
    TEST_ASSERT(gui.find("LB_GETTEXTLEN") != std::string::npos, "Queue painter must use LB_GETTEXTLEN");
    TEST_ASSERT(gui.find("LB_GETSELCOUNT") != std::string::npos && gui.find("LB_GETSELITEMS") != std::string::npos,
                "Remove must use multiselect APIs");
    TEST_ASSERT(gui.find("recompute_all_outputs();") != std::string::npos, "Outputs must be recomputed before conversion");
    TEST_ASSERT(gui.find("output collision") != std::string::npos, "Preflight must detect output collisions");
    std::string types = read_src("src/bzconverter/core/types.hpp");
    TEST_ASSERT(types.find("bak += L\".bak_\" + std::to_wstring(seq) + L\".bzconv\"") != std::string::npos,
                "Non-Windows backup name must use wide strings consistently");
    return true;
}

namespace {

struct GlbAccess {
    bz::Json json;
    std::vector<uint8_t> bin;
    bool parsed = false;
};

uint32_t read_u32_le(const std::vector<uint8_t>& bytes, size_t off) {
    return uint32_t(bytes[off]) | (uint32_t(bytes[off+1]) << 8) |
           (uint32_t(bytes[off+2]) << 16) | (uint32_t(bytes[off+3]) << 24);
}

bool parse_glb(const std::vector<uint8_t>& bytes, GlbAccess& out, std::string& err) {
    if (bytes.size() < 20 || read_u32_le(bytes, 0) != 0x46546C67) { err = "not a GLB"; return false; }
    size_t off = 12;
    while (off + 8 <= bytes.size()) {
        uint32_t len = read_u32_le(bytes, off), type = read_u32_le(bytes, off + 4);
        if (off + 8 + len > bytes.size()) { err = "chunk overruns"; return false; }
        if (type == 0x4E4F534A) {
            std::string js(reinterpret_cast<const char*>(bytes.data() + off + 8), len);
            if (!bz::Json::parse(js, out.json, err)) return false;
        } else if (type == 0x004E4942) {
            out.bin.assign(bytes.begin() + off + 8, bytes.begin() + off + 8 + len);
        }
        off += 8 + len;
    }
    out.parsed = true;
    return true;
}

const unsigned char kMinimalPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
    0x44, 0xAE, 0x42, 0x60, 0x82
};

bool write_minimal_png(const fs::path& path) {
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(path, kMinimalPng, sizeof(kMinimalPng), err), "write minimal png");
    return true;
}

bool make_skin_scene(bz::Scene& scene, size_t inf_count) {
    scene.source_name = "p0_skin_fixture";
    for (size_t i = 0; i < 12; ++i) {
        bz::Node n; n.name = "bone" + std::to_string(i); n.is_bone = true;
        scene.nodes.push_back(n);
    }
    bz::Mesh m;
    m.name = "skinned"; m.is_skinned = true;
    m.vertices.resize(1);
    m.vertices[0].pos = {0, 0, 0};
    m.vertices[0].normal = {0, 1, 0};
    m.indices = {0, 0, 0};
    std::vector<bz::BoneWeight> wl;
    for (size_t i = 0; i < inf_count; ++i) wl.push_back({static_cast<int>(i), 1.0f / float(inf_count)});
    m.skin_weights.push_back(wl);
    scene.meshes.push_back(m);
    scene.normalize_skin_weights();
    return true;
}

// Read one vertex WEIGHTS_n accessor directly from the GLB binary chunk.
float read_weight(const GlbAccess& g, const std::string& attr, size_t slot) {
    const auto* meshes = g.json.get("meshes");
    if (!meshes || meshes->as_arr().empty()) return -1.0f;
    const auto* prims = meshes->as_arr()[0].get("primitives");
    if (!prims || prims->as_arr().empty()) return -1.0f;
    const auto* attrs = prims->as_arr()[0].get("attributes");
    if (!attrs) return -1.0f;
    const auto* acc_ref = attrs->get(attr);
    if (!acc_ref) return 0.0f;
    const auto* accessors = g.json.get("accessors");
    if (!accessors) return -1.0f;
    const auto& acc = accessors->as_arr()[acc_ref->as_int()];
    const auto* bvs = g.json.get("bufferViews");
    const auto& bv = bvs->as_arr()[acc.get("bufferView")->as_int()];
    size_t off = size_t(bv.get("byteOffset")->as_num()) + slot * 4;
    if (off + 4 > g.bin.size()) return -1.0f;
    float f;
    std::memcpy(&f, g.bin.data() + off, 4);
    return f;
}

size_t weight_set_count(const GlbAccess& g) {
    const auto* meshes = g.json.get("meshes");
    if (!meshes || meshes->as_arr().empty()) return 0;
    const auto* prims = meshes->as_arr()[0].get("primitives");
    if (!prims || prims->as_arr().empty()) return 0;
    const auto* attrs = prims->as_arr()[0].get("attributes");
    if (!attrs) return 0;
    size_t n = 0;
    while (attrs->get("WEIGHTS_" + std::to_string(n))) ++n;
    return n;
}

} // namespace

bool test_glb_exact_influence_limit_and_renormalization() {
    const size_t source_counts[] = {1, 4, 5, 8, 12};
    const size_t limits[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    for (size_t count : source_counts) {
        for (size_t limit : limits) {
            bz::Scene scene;
            make_skin_scene(scene, count);
            bz::ExportOptions opts;
            opts.include_materials = false;
            opts.include_textures = false;
            opts.overwrite = true;
            opts.glb_max_influences = limit;
            fs::path out = fs::temp_directory_path() / ("bzconv_p0_limit_" + std::to_string(count) + "_" + std::to_string(limit) + ".glb");
            std::error_code ec;
            fs::remove(out, ec);
            std::vector<bz::Diagnostic> diags;
            std::string err;
            TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err),
                        ("export failed: " + err).c_str());
            std::vector<uint8_t> bytes;
            TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "read GLB");
            GlbAccess glb;
            TEST_ASSERT(parse_glb(bytes, glb, err), ("GLB parse: " + err).c_str());
            size_t sets = weight_set_count(glb);
            size_t expected_inf = (limit == 0) ? count : std::min(count, limit);
            size_t expected_sets = std::max<size_t>(1, (expected_inf + 3) / 4);
            TEST_ASSERT(sets == expected_sets, ("set count " + std::to_string(sets) + " expected " + std::to_string(expected_sets)).c_str());
            float sum = 0.0f;
            float nonzero = 0.0f;
            for (size_t s = 0; s < sets; ++s) {
                for (size_t k = 0; k < 4; ++k) {
                    float w = read_weight(glb, "WEIGHTS_" + std::to_string(s), k);
                    TEST_ASSERT(w >= 0.0f, "negative exported weight");
                    sum += w;
                    if (w > 1e-6f) nonzero += 1.0f;
                }
            }
            TEST_ASSERT(std::fabs(sum - 1.0f) < 1e-4f,
                        ("weight sum " + std::to_string(sum) + " for count " + std::to_string(count) + " limit " + std::to_string(limit)).c_str());
            TEST_ASSERT(nonzero == float(expected_inf),
                        ("nonzero count " + std::to_string(nonzero) + " expected " + std::to_string(expected_inf)).c_str());
            bool should_lose = limit > 0 && count > limit;
            bool has_loss = false;
            for (const auto& d : diags) if (d.code == "GLB_INFLUENCE_LIMIT_LOSS") has_loss = true;
            TEST_ASSERT(has_loss == should_lose, "loss diagnostic mismatch");
            fs::remove(out, ec);
        }
    }
    return true;
}

bool test_glb_influence_edge_cases() {
    bz::Scene scene;
    make_skin_scene(scene, 8);
    // Raw list exercises helper filtering: duplicate joints + NaN/Inf +
    // negative + zero + unmapped + unsorted.
    std::vector<bz::BoneWeight> wl = {
        {3, 0.2f}, {99, 0.5f}, {3, 0.1f}, {1, std::numeric_limits<float>::quiet_NaN()},
        {2, std::numeric_limits<float>::infinity()}, {4, -0.3f}, {5, 0.0f},
        {6, 0.4f}, {0, 0.2f}, {7, 0.1f}, {8, 0.2f}, {9, 0.1f}
    };
    std::unordered_map<int, uint16_t> n2j;
    for (size_t i = 0; i < scene.nodes.size(); ++i) n2j[int(i)] = uint16_t(i);
    auto inf = bz::select_skin_influences(wl, n2j, 5);
    TEST_ASSERT(inf.size() == 5, "exact limit 5 must yield 5 influences");
    float sum = 0.0f;
    for (const auto& e : inf) {
        TEST_ASSERT(std::isfinite(e.weight) && e.weight > 0.0f, "finite positive kept weight");
        sum += e.weight;
    }
    TEST_ASSERT(std::fabs(sum - 1.0f) < 1e-4f, "kept influences sum to 1");
    bool merged = false;
    for (const auto& e : inf) {
        TEST_ASSERT(e.joint != 99, "unmapped joint must be dropped");
        if (e.joint == 3) { merged = true; TEST_ASSERT(e.weight > 0.24f, "duplicate joint 3 merged"); }
    }
    TEST_ASSERT(merged, "duplicate joint 3 must survive");
    return true;
}

bool test_status_glb_loss_forces_partial() {
    bz::Scene scene;
    make_skin_scene(scene, 8);
    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;
    opts.glb_max_influences = 4;
    fs::path out = fs::temp_directory_path() / "bzconv_p0_loss_status.glb";
    std::error_code ec;
    fs::remove(out, ec);
    // Verify the classifier contract: the same diagnostic produced by the
    // exporter must be recognized as a PARTIAL-forcing policy entry.
    std::vector<bz::Diagnostic> diags;
    std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), "export");
    bool loss = false;
    for (const auto& d : diags) if (d.code == "GLB_INFLUENCE_LIMIT_LOSS") loss = true;
    TEST_ASSERT(loss, "GLB_INFLUENCE_LIMIT_LOSS must be emitted");
    bz::ExportResult r;
    r.status = bz::JobStatus::Success;
    r.result_class = bz::ResultClass::VerifiedSuccess;
    r.diagnostics = diags;
    bz::adjudicate_result(r, opts, scene.compute_stats());
    TEST_ASSERT(r.status == bz::JobStatus::Partial, "loss diagnostic must make status PARTIAL");
    TEST_ASSERT(r.result_class == bz::ResultClass::Partial, "loss diagnostic must make class PARTIAL");
    TEST_ASSERT(r.summary.find("GLB_INFLUENCE_LIMIT_LOSS") != std::string::npos,
                "summary must name the loss reason");
    fs::remove(out, ec);
    return true;
}

bool test_gltf_sidecar_overwrite_policy() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_sidecars";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path gltf = dir / "model.gltf";
    fs::path bin = dir / "model.bin";

    // Case 1: .gltf absent, .bin exists with user data => overwrite=false fails.
    const std::string sentinel = "SENTINEL_BIN_USER_DATA";
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(bin, sentinel.data(), sentinel.size(), err), "write sentinel");
    bz::Scene scene; make_skin_scene(scene, 4);
    bz::ExportOptions opts;
    opts.include_materials = false; opts.include_textures = false; opts.overwrite = false;
    std::vector<bz::Diagnostic> diags;
    bool ok = bz::export_scene_gltf(scene, gltf, false, opts, diags, err);
    TEST_ASSERT(!ok, "GLTF export must refuse when sidecar exists and overwrite disabled");
    TEST_ASSERT(!fs::exists(gltf, ec), "no GLTF should be created");
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(bin, bytes, err), "read sentinel");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == sentinel, "sentinel .bin must be untouched");

    // Case 2: overwrite=true writes the complete bundle.
    opts.overwrite = true;
    ok = bz::export_scene_gltf(scene, gltf, false, opts, diags, err);
    TEST_ASSERT(ok, ("GLTF overwrite export: " + err).c_str());
    TEST_ASSERT(fs::exists(gltf, ec) && fs::exists(bin, ec), "both artifacts must exist");
    fs::remove_all(dir, ec);
    return true;
}

bool test_obj_sidecar_overwrite_policy() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_obj_sidecars";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path obj = dir / "model.obj";
    fs::path mtl = dir / "model.mtl";
    const std::string sentinel = "SENTINEL_MTL_USER_DATA";
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(mtl, sentinel.data(), sentinel.size(), err), "write mtl sentinel");
    bz::Scene scene;
    scene.source_name = "obj_sidecar";
    bz::Node n; n.name = "root"; scene.nodes.push_back(n);
    bz::Mesh m; m.name = "mesh"; m.vertices.resize(3); m.indices = {0,1,2};
    scene.meshes.push_back(m);
    bz::Material mat; mat.name = "mat"; scene.materials.push_back(mat);
    scene.meshes[0].triangle_materials = {0};
    bz::ExportOptions opts;
    opts.include_materials = true; opts.include_textures = false; opts.overwrite = false;
    std::vector<bz::Diagnostic> diags;
    bool ok = bz::export_scene_obj(scene, obj, opts, diags, err);
    TEST_ASSERT(!ok, "OBJ export must refuse when .mtl exists and overwrite disabled");
    TEST_ASSERT(!fs::exists(obj, ec), "no OBJ should be created");
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(mtl, bytes, err), "read mtl sentinel");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == sentinel, "sentinel .mtl must be untouched");
    fs::remove_all(dir, ec);
    return true;
}

bool test_gltf_texture_sidecar_overwrite_policy() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_tex_sidecars";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path src_tex = dir / "source.png";
    write_minimal_png(src_tex);
    std::string err;

    bz::Scene scene;
    make_skin_scene(scene, 4);
    bz::Material mat; mat.name = "mat";
    mat.diffuse_tex.resolved_path = bz::utf8_from_path(src_tex);
    scene.materials.push_back(mat);

    bz::ExportOptions opts;
    opts.include_materials = true;
    opts.include_textures = true;
    opts.embed_textures = false;
    opts.overwrite = false;

    fs::path out = dir / "model.gltf";
    fs::path tex_dir = dir / "model_textures";
    fs::path staged = tex_dir / "source.png";
    const std::string sentinel = "USER_TEXTURE_SENTINEL";
    fs::create_directories(tex_dir, ec);
    TEST_ASSERT(bz::write_file_atomic(staged, sentinel.data(), sentinel.size(), err), "write staged sentinel");

    std::vector<bz::Diagnostic> diags;
    bool ok = bz::export_scene_gltf(scene, out, false, opts, diags, err);
    TEST_ASSERT(!ok, ("GLTF must refuse when an external texture target exists without overwrite: " + err).c_str());
    TEST_ASSERT(!fs::exists(out, ec), "no GLTF document should be created");
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(staged, bytes, err), "read staged sentinel");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == sentinel, "user texture must be untouched");

    opts.overwrite = true;
    ok = bz::export_scene_gltf(scene, out, false, opts, diags, err);
    TEST_ASSERT(ok, ("GLTF texture overwrite: " + err).c_str());
    TEST_ASSERT(fs::exists(out, ec) && fs::exists(staged, ec), "both document and texture must exist");
    fs::remove_all(dir, ec);
    return true;
}

bool test_companion_multiclip_same_bone() {
    bz::Scene anim, base;
    auto mk = [](const char* n) { bz::Node x; x.name = n; return x; };
    anim.nodes.push_back(mk("turret"));  // 0
    anim.nodes.push_back(mk("barrel"));  // 1
    base.nodes.push_back(mk("body"));    // 0
    base.nodes.push_back(mk("barrel"));  // 1
    base.nodes.push_back(mk("turret"));  // 2
    base.meshes.resize(1);
    bz::AnimationClip walk; walk.name = "walk";
    bz::AnimTrack t0; t0.node_index = 0; t0.node_name = "turret";
    bz::AnimTrack t1; t1.node_index = 1; t1.node_name = "barrel";
    walk.tracks = {t0, t1};
    bz::AnimationClip turn; turn.name = "turn";
    turn.tracks = {t0, t1, t0}; // same two bones again, repeated track
    anim.animations = {walk, turn};
    std::unordered_map<int,int> map;
    std::string reason;
    TEST_ASSERT(bz::build_companion_node_map(anim, base, map, reason),
                ("multi-clip mapping failed: " + reason).c_str());
    TEST_ASSERT(map.size() == 2, "two unique source nodes");
    TEST_ASSERT(map[0] == 2 && map[1] == 1, "correct targets");
    // Missing target bone must still fail even with multi-clip duplication.
    bz::Scene bad = anim;
    bad.nodes.push_back(mk("missing_bone"));
    bz::AnimationClip clip3; clip3.name = "third";
    bz::AnimTrack tm; tm.node_index = 2; tm.node_name = "missing_bone";
    clip3.tracks = {tm};
    bad.animations.push_back(clip3);
    map.clear(); reason.clear();
    TEST_ASSERT(!bz::build_companion_node_map(bad, base, map, reason), "missing target must fail");
    return true;
}

std::vector<float> read_accessor_vec(const GlbAccess& g, int accessor_index) {
    std::vector<float> out;
    const auto* accessors = g.json.get("accessors");
    if (!accessors) return out;
    const auto& acc = accessors->as_arr()[accessor_index];
    std::string type = acc.get("type")->as_str();
    int comps = type == "VEC3" ? 3 : (type == "VEC2" ? 2 : 1);
    size_t count = size_t(acc.get("count")->as_num());
    const auto* bvs = g.json.get("bufferViews");
    const auto& bv = bvs->as_arr()[acc.get("bufferView")->as_int()];
    size_t base = size_t(bv.get("byteOffset")->as_num());
    out.resize(count * comps);
    for (size_t i = 0; i < out.size(); ++i) {
        float f;
        std::memcpy(&f, g.bin.data() + base + i * 4, 4);
        out[i] = f;
    }
    return out;
}

bool test_glb_rotated_model_space_normal() {
    bz::Scene scene;
    scene.source_name = "rotated_normal";
    bz::Node n;
    n.name = "rotated";
    bz::Quat q; q.w = 0.70710678f; q.z = 0.70710678f;
    n.local_matrix = bz::Mat4::from_bz_trs({0,0,0}, q);
    scene.nodes.push_back(n);
    bz::Mesh m;
    m.name = "rigid";
    m.vertices_in_model_space = true;
    m.vertices.resize(1);
    m.vertices[0].pos = {0, 1, 0};
    m.vertices[0].normal = {0, 1, 0};
    m.indices = {0, 0, 0};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.root_nodes.push_back(0);

    bz::ExportOptions opts;
    opts.include_materials = false; opts.include_textures = false; opts.overwrite = true;
    fs::path out = fs::temp_directory_path() / "bzconv_p0_rotated_normal.glb";
    std::error_code ec; fs::remove(out, ec);
    std::vector<bz::Diagnostic> diags; std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), ("export: " + err).c_str());
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "read GLB");
    GlbAccess glb;
    TEST_ASSERT(parse_glb(bytes, glb, err), ("parse: " + err).c_str());
    const auto* meshes = glb.json.get("meshes");
    const auto* prims = meshes->as_arr()[0].get("primitives");
    const auto* attrs = prims->as_arr()[0].get("attributes");
    auto norm = read_accessor_vec(glb, attrs->get("NORMAL")->as_int());
    TEST_ASSERT(norm.size() == 3, "one normal");
    // World-space contract (SAIHANDOFF-11 P0-1): the exported local normal
    // must reconstruct, via the node's glTF TRS, to the SOURCE world normal
    // (0,1,0). The pre-fix code wrote (1,0,0), which reconstructs to
    // (0,-1,0) — the inversion the external audit measured. The correct
    // local value is (-1,0,0).
    TEST_ASSERT(std::fabs(norm[0] + 1.0f) < 1e-3f && std::fabs(norm[1]) < 1e-3f && std::fabs(norm[2]) < 1e-3f,
                ("normal local " + std::to_string(norm[0]) + "," + std::to_string(norm[1]) + "," + std::to_string(norm[2])).c_str());
    fs::remove(out, ec);
    return true;
}

bool test_glb_parent_after_child_order() {
    bz::Scene scene;
    scene.source_name = "parent_after_child";
    bz::Node child; child.name = "child"; child.parent_index = 1;
    child.local_matrix.m[12] = 5.0f;
    bz::Node parent; parent.name = "parent"; parent.local_matrix.m[12] = 7.0f;
    scene.nodes.push_back(child);
    scene.nodes.push_back(parent);
    bz::Mesh m; m.name = "mesh"; m.vertices_in_model_space = true;
    m.vertices.resize(1); m.vertices[0].pos = {12.0f, 0, 0}; m.indices = {0,0,0};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.root_nodes.push_back(1);

    bz::ExportOptions opts;
    opts.include_materials = false; opts.include_textures = false; opts.overwrite = true;
    fs::path out = fs::temp_directory_path() / "bzconv_p0_parent_order.glb";
    std::error_code ec; fs::remove(out, ec);
    std::vector<bz::Diagnostic> diags; std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), ("export: " + err).c_str());
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "read GLB");
    GlbAccess glb;
    TEST_ASSERT(parse_glb(bytes, glb, err), ("parse: " + err).c_str());
    const auto* meshes = glb.json.get("meshes");
    const auto* prims = meshes->as_arr()[0].get("primitives");
    const auto* attrs = prims->as_arr()[0].get("attributes");
    auto pos = read_accessor_vec(glb, attrs->get("POSITION")->as_int());
    TEST_ASSERT(pos.size() == 3, "position");
    TEST_ASSERT(std::fabs(pos[0]) < 1e-4f, ("local X " + std::to_string(pos[0])).c_str());
    fs::remove(out, ec);
    return true;
}

bool test_glb_skinned_model_space_bind_pose() {
    bz::Scene scene;
    scene.source_name = "skinned_bind";
    bz::Node mesh_node; mesh_node.name = "mesh"; mesh_node.local_matrix.m[12] = 5.0f;
    bz::Node bone; bone.name = "bone"; bone.is_bone = true; bone.has_inverse_bind = true;
    scene.nodes.push_back(mesh_node);
    scene.nodes.push_back(bone);
    bz::Mesh m; m.name = "skinned"; m.is_skinned = true; m.vertices_in_model_space = true;
    m.vertices.resize(1); m.vertices[0].pos = {5,0,0}; m.vertices[0].normal = {0,1,0};
    m.indices = {0,0,0};
    m.skin_weights.push_back({{1, 1.0f}});
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.root_nodes.push_back(0);
    scene.root_nodes.push_back(1);

    bz::ExportOptions opts;
    opts.include_materials = false; opts.include_textures = false; opts.overwrite = true;
    fs::path out = fs::temp_directory_path() / "bzconv_p0_skinned_bind.glb";
    std::error_code ec; fs::remove(out, ec);
    std::vector<bz::Diagnostic> diags; std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), ("export: " + err).c_str());
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "read GLB");
    GlbAccess glb;
    TEST_ASSERT(parse_glb(bytes, glb, err), ("parse: " + err).c_str());
    const auto* meshes = glb.json.get("meshes");
    const auto* prims = meshes->as_arr()[0].get("primitives");
    const auto* attrs = prims->as_arr()[0].get("attributes");
    auto pos = read_accessor_vec(glb, attrs->get("POSITION")->as_int());
    TEST_ASSERT(pos.size() == 3, "position");
    TEST_ASSERT(std::fabs(pos[0] - 5.0f) < 1e-4f, ("bind X " + std::to_string(pos[0])).c_str());
    const auto* skins = glb.json.get("skins");
    TEST_ASSERT(skins && !skins->as_arr().empty(), "skin");
    const auto* skel = skins->as_arr()[0].get("skeleton");
    TEST_ASSERT(!skel || skel->as_int() == 1, "skeleton root must not be mesh node");
    bool bind_diag = false;
    for (const auto& d : diags) if (d.code == "GLB_SKINNED_MODEL_SPACE") bind_diag = true;
    TEST_ASSERT(bind_diag, "skinned model-space diagnostic");
    fs::remove(out, ec);
    return true;
}

bool test_glb_invalid_images_rejected() {
    static const unsigned char bad_png[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
        0xDE,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
    };
    static const unsigned char bad_jpeg[] = {0xFF,0xD8,0x00,0x01,0x02,0xFF,0xD9};
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_invalid_images";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    fs::path pp = dir / "bad.png", jp = dir / "bad.jpg";
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(pp, bad_png, sizeof(bad_png), err), "write bad png");
    TEST_ASSERT(bz::write_file_atomic(jp, bad_jpeg, sizeof(bad_jpeg), err), "write bad jpeg");

    bz::Scene scene; make_skin_scene(scene, 4);
    bz::Material mat; mat.name = "mat"; scene.materials.push_back(mat);
    bz::ExportOptions opts;
    opts.include_materials = true; opts.include_textures = true; opts.overwrite = true;
    fs::path out = dir / "model.glb";
    std::vector<bz::Diagnostic> diags;
    for (int which = 0; which < 2; ++which) {
        scene.materials[0].diffuse_tex.resolved_path = bz::utf8_from_path(which == 0 ? pp : jp);
        diags.clear();
        fs::remove(out, ec);
        TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), "export");
        bool invalid = false;
        for (const auto& d : diags) if (d.code == "GLTF_TEXTURE_INVALID") invalid = true;
        TEST_ASSERT(invalid, std::string(which == 0 ? "invalid PNG diagnosed" : "invalid JPEG diagnosed"));
        bz::ExportResult r; r.status = bz::JobStatus::Success; r.result_class = bz::ResultClass::SuccessUnverified;
        r.diagnostics = diags;
        bz::adjudicate_result(r, opts, scene.compute_stats());
        TEST_ASSERT(r.result_class == bz::ResultClass::Partial, "invalid image must be PARTIAL");
    }
    fs::remove_all(dir, ec);
    return true;
}

bool test_glb_model_space_no_double_transform() {
    bz::Scene scene;
    scene.source_name = "model_space";
    bz::Node root;
    root.name = "root";
    root.local_matrix.m[12] = 5.0f;
    root.local_matrix.m[13] = 2.0f;
    scene.nodes.push_back(root);

    bz::Mesh m;
    m.name = "rigid";
    m.vertices_in_model_space = true;
    m.vertices.resize(3);
    m.vertices[0].pos = {5.0f, 2.0f, 0.0f};
    m.vertices[1].pos = {6.0f, 2.0f, 0.0f};
    m.vertices[2].pos = {5.0f, 3.0f, 0.0f};
    m.indices = {0, 1, 2};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.root_nodes.push_back(0);

    bz::ExportOptions opts;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;
    fs::path out = fs::temp_directory_path() / "bzconv_p0_model_space.glb";
    std::error_code ec;
    fs::remove(out, ec);
    std::vector<bz::Diagnostic> diags;
    std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), ("export: " + err).c_str());
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(out, bytes, err), "read GLB");
    GlbAccess glb;
    TEST_ASSERT(parse_glb(bytes, glb, err), ("parse: " + err).c_str());
    const auto* accessors = glb.json.get("accessors");
    const auto* meshes = glb.json.get("meshes");
    TEST_ASSERT(accessors && meshes && !meshes->as_arr().empty(), "GLB accessors/meshes");
    const auto* prims = meshes->as_arr()[0].get("primitives");
    const auto* attrs = prims->as_arr()[0].get("attributes");
    const auto& acc = accessors->as_arr()[attrs->get("POSITION")->as_int()];
    const auto* bvs = glb.json.get("bufferViews");
    const auto& bv = bvs->as_arr()[acc.get("bufferView")->as_int()];
    size_t off = size_t(bv.get("byteOffset")->as_num());
    float px, py;
    std::memcpy(&px, glb.bin.data() + off, 4);
    std::memcpy(&py, glb.bin.data() + off + 4, 4);
    // POSITION is node-local; node translation is (5,2,0). World X/Y must be (5,2).
    TEST_ASSERT(std::fabs((px + 5.0f) - 5.0f) < 1e-4f, ("world X " + std::to_string(px + 5.0f)).c_str());
    TEST_ASSERT(std::fabs((py + 2.0f) - 2.0f) < 1e-4f, ("world Y " + std::to_string(py + 2.0f)).c_str());
    fs::remove(out, ec);
    return true;
}

bool test_input_output_identity_guard() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_identity";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path src = dir / "model.xsi";
    std::string err;
    TEST_ASSERT(bz::write_file_atomic(src, "SENTINEL", 8, err), "write source sentinel");

    bz::ExportOptions opts;
    opts.format = bz::ExportFormat::GLB;
    opts.include_materials = false;
    opts.include_textures = false;
    opts.overwrite = true;
    bz::MaterialResolver resolver;
    bz::ExportResult r = bz::convert_single_file(src, src, opts, resolver);
    TEST_ASSERT(r.status == bz::JobStatus::Failed, "same path must fail even with overwrite=true");
    TEST_ASSERT(r.result_class == bz::ResultClass::Failed, "class must be FAILED");
    bool identity_diag = false;
    for (const auto& d : r.diagnostics) if (d.code == "INPUT_OUTPUT_IDENTITY") identity_diag = true;
    TEST_ASSERT(identity_diag, "identity diagnostic must be present");
    std::vector<uint8_t> bytes;
    TEST_ASSERT(bz::read_file_bytes(src, bytes, err), "read sentinel");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == "SENTINEL", "source must be untouched");

    // Case-alias on Windows: same identity under different capitalization.
    fs::path alias = dir / "MODEL.XSI";
    r = bz::convert_single_file(src, alias, opts, resolver);
    TEST_ASSERT(r.status == bz::JobStatus::Failed, "case alias must fail");
    fs::remove_all(dir, ec);
    return true;
}

bool test_glb_texture_loss_diagnostics() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_texdiag";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path missing = dir / "missing.png";
    fs::path corrupt = dir / "corrupt.png";

    bz::Scene scene;
    make_skin_scene(scene, 4);
    bz::Material mat; mat.name = "mat";
    mat.diffuse_tex.resolved_path = bz::utf8_from_path(missing);
    scene.materials.push_back(mat);

    bz::ExportOptions opts;
    opts.include_materials = true;
    opts.include_textures = true;
    opts.overwrite = true;
    fs::path out = dir / "model.glb";
    std::vector<bz::Diagnostic> diags;
    std::string err;
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), "missing texture export should still succeed");
    bool missing_diag = false;
    for (const auto& d : diags) if (d.code == "GLTF_TEXTURE_MISSING") missing_diag = true;
    TEST_ASSERT(missing_diag, "missing texture must be diagnosed");
    bz::ExportResult r;
    r.status = bz::JobStatus::Success;
    r.result_class = bz::ResultClass::SuccessUnverified;
    r.diagnostics = diags;
    bz::adjudicate_result(r, opts, scene.compute_stats());
    TEST_ASSERT(r.result_class == bz::ResultClass::Partial, "missing texture must be PARTIAL");

    std::string err2;
    TEST_ASSERT(bz::write_file_atomic(corrupt, "NOT_A_PNG", 9, err2), "write corrupt texture");
    scene.materials[0].diffuse_tex.resolved_path = bz::utf8_from_path(corrupt);
    diags.clear();
    fs::remove(out, ec);
    TEST_ASSERT(bz::export_scene_gltf(scene, out, true, opts, diags, err), "corrupt texture export succeeds with diagnostic");
    bool invalid_diag = false;
    for (const auto& d : diags) if (d.code == "GLTF_TEXTURE_INVALID") invalid_diag = true;
    TEST_ASSERT(invalid_diag, "corrupt texture must be diagnosed");
    fs::remove_all(dir, ec);
    return true;
}

bool test_output_bundle_cancel_reason() {
    fs::path dir = fs::temp_directory_path() / "bzconv_p0_bundle_cancel";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    bz::OutputBundle bundle;
    std::vector<uint8_t> data = {'X'};
    bundle.add(dir / "model.bin", data, "binary");
    std::atomic<bool> cancel{true};
    std::string err;
    bool ok = bundle.commit(false, &cancel, err);
    TEST_ASSERT(!ok, "cancel must fail commit");
    TEST_ASSERT(err == "CANCELLED_BEFORE_COMMIT", ("empty/changed cancel reason: '" + err + "'").c_str());
    TEST_ASSERT(!fs::exists(dir / "model.bin", ec), "cancelled bundle must not publish");
    fs::remove_all(dir, ec);
    return true;
}

// §12: a successful commit leaves no staging dir or journal behind, and
// every declared final file is present with the staged bytes.
bool test_output_bundle_commit_cleans_durable_stage() {
    fs::path dir = fs::temp_directory_path() / "bzconv_s12_commit_clean";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    bz::OutputBundle bundle;
    bundle.add(dir / "scene.gltf", {'G', 'L', 'T', 'F'}, "glTF document");
    bundle.add(dir / "scene.bin", {'B', 'I', 'N', 0}, "glTF binary buffer");
    std::string err;
    TEST_ASSERT(bundle.commit(false, nullptr, err), ("commit failed: " + err).c_str());
    TEST_ASSERT(fs::exists(dir / "scene.gltf", ec), "glTF document must be published");
    TEST_ASSERT(fs::exists(dir / "scene.bin", ec), "glTF binary must be published");
    size_t stages = 0;
    for (const auto& it : fs::directory_iterator(dir, ec))
        if (it.is_directory()) ++stages;
    TEST_ASSERT(stages == 0, "successful commit must remove staging directories");
    fs::remove_all(dir, ec);
    return true;
}

// §12: restart recovery of a crash between the first and second commit of
// a two-file bundle. The journal (committedAll=false, first entry committed)
// plus staged originals must restore the old file and remove the new one,
// with no help from the crashed process.
bool test_output_bundle_recover_interrupted_commit() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bzconv_s12_recovery";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path obj = dir / "scene.obj";
    fs::path mtl = dir / "scene.mtl";
    { std::ofstream f(obj, std::ios::binary); f << "OLD_OBJ_CONTENT"; }
    // Simulate a crashed first commit: old file replaced, second file not yet
    // written, journal + staged backup still on disk.
    fs::path stage = dir / ".scene.obj.bzconv-stage";
    fs::create_directories(stage, ec);
    bz::Json je1 = bz::Json::object();
    je1["role"] = "OBJ document";
    je1["finalPath"] = bz::utf8_from_path(obj);
    je1["existed"] = true;
    je1["stagedTemp"] = bz::utf8_from_path(stage / "0_scene.obj");
    je1["originalBackup"] = bz::utf8_from_path(stage / "0_orig_scene.obj");
    je1["committed"] = true;
    bz::Json je2 = bz::Json::object();
    je2["role"] = "OBJ material library";
    je2["finalPath"] = bz::utf8_from_path(mtl);
    je2["existed"] = false;
    je2["stagedTemp"] = bz::utf8_from_path(stage / "1_scene.mtl");
    je2["originalBackup"] = bz::Json();
    je2["committed"] = false;
    bz::Json jr = bz::Json::object();
    jr["format"] = "bzconv-bundle-journal";
    jr["version"] = 1;
    jr["committedAll"] = false;
    jr["entries"] = bz::Json::array();
    jr["entries"].push_back(je1);
    jr["entries"].push_back(je2);
    std::string jtext = jr.dump(2);
    { std::ofstream f(stage / "bundle-journal.json", std::ios::binary); f << jtext; }
    { std::ofstream f(stage / "0_orig_scene.obj", std::ios::binary); f << "OLD_OBJ_CONTENT"; }
    { std::ofstream f(obj, std::ios::binary); f << "NEW_OBJ_CONTENT"; }
    std::string report;
    TEST_ASSERT(bz::OutputBundle::recover_pending(dir, report), ("recovery failed: " + report).c_str());
    std::vector<uint8_t> obj_after, mtl_check;
    std::string err;
    TEST_ASSERT(bz::read_file_bytes(obj, obj_after, err), "restored OBJ must exist");
    TEST_ASSERT(std::string(obj_after.begin(), obj_after.end()) == "OLD_OBJ_CONTENT",
                "original OBJ bytes must be restored byte-for-byte");
    TEST_ASSERT(!fs::exists(mtl, ec), "output created by the crashed commit must be removed");
    TEST_ASSERT(!fs::exists(stage, ec), "recovered staging dir must be removed");

    // Second crash shape: both files committed, second one did not exist
    // before the bundle. Recovery must delete it (not merely leave it).
    fs::path stage2 = dir / ".scene2.obj.bzconv-stage";
    fs::create_directories(stage2, ec);
    fs::path obj2 = dir / "scene2.obj";
    fs::path mtl2 = dir / "scene2.mtl";
    bz::Json b1 = bz::Json::object();
    b1["role"] = "OBJ document"; b1["finalPath"] = bz::utf8_from_path(obj2);
    b1["existed"] = false; b1["stagedTemp"] = ""; b1["originalBackup"] = bz::Json(); b1["committed"] = true;
    bz::Json b2 = bz::Json::object();
    b2["role"] = "OBJ material library"; b2["finalPath"] = bz::utf8_from_path(mtl2);
    b2["existed"] = false; b2["stagedTemp"] = ""; b2["originalBackup"] = bz::Json(); b2["committed"] = true;
    bz::Json j2 = bz::Json::object();
    j2["format"] = "bzconv-bundle-journal"; j2["version"] = 1; j2["committedAll"] = false;
    j2["entries"] = bz::Json::array(); j2["entries"].push_back(b1); j2["entries"].push_back(b2);
    std::string j2text = j2.dump(2);
    { std::ofstream f(stage2 / "bundle-journal.json", std::ios::binary); f << j2text; }
    { std::ofstream f(obj2, std::ios::binary); f << "NEW2"; }
    { std::ofstream f(mtl2, std::ios::binary); f << "NEW2_MTL"; }
    report.clear();
    TEST_ASSERT(bz::OutputBundle::recover_pending(dir, report), ("recovery 2 failed: " + report).c_str());
    TEST_ASSERT(!fs::exists(obj2, ec) && !fs::exists(mtl2, ec),
                "both outputs created by a crashed commit must be removed");
    fs::remove_all(dir, ec);
    return true;
}

// T-12: drive the real `bzconv recover <dir>` CLI entry point, not just the
// OutputBundle core. A synthetic crashed-commit journal + staged original
// must be rolled back by the shipped executable with exit code 0.
bool test_cli_recover_dispatch() {
    namespace fs = std::filesystem;
    fs::path exe;
    for (const fs::path candidate : {fs::path("bzconv.exe"), fs::path("build/bzconv.exe"),
                                     fs::path("../build/bzconv.exe")}) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) { exe = candidate; break; }
    }
    TEST_ASSERT(!exe.empty(), "bzconv.exe not found for CLI recovery test");
    std::error_code ec;
    exe = fs::absolute(exe, ec).lexically_normal();

    fs::path dir = fs::temp_directory_path() / "bzconv_t12_cli_recover";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path obj = dir / "scene.obj";
    fs::path stage = dir / ".scene.obj.bzconv-stage";
    fs::create_directories(stage, ec);
    bz::Json je = bz::Json::object();
    je["role"] = "OBJ document";
    je["finalPath"] = bz::utf8_from_path(obj);
    je["existed"] = true;
    je["stagedTemp"] = "";
    je["originalBackup"] = bz::utf8_from_path(stage / "orig.bin");
    je["committed"] = true;
    bz::Json jr = bz::Json::object();
    jr["format"] = "bzconv-bundle-journal";
    jr["version"] = 1;
    jr["committedAll"] = false;
    jr["entries"] = bz::Json::array();
    jr["entries"].push_back(je);
    { std::ofstream f(stage / "bundle-journal.json", std::ios::binary); f << jr.dump(2); }
    { std::ofstream f(stage / "orig.bin", std::ios::binary); f << "ORIGINAL"; }
    { std::ofstream f(obj, std::ios::binary); f << "MUTATED"; }

    std::string cmd = exe.string() + " recover " + dir.string();
    int rc = std::system(cmd.c_str());
    TEST_ASSERT(rc == 0, ("bzconv recover exited " + std::to_string(rc)).c_str());
    std::vector<uint8_t> bytes;
    std::string err;
    TEST_ASSERT(bz::read_file_bytes(obj, bytes, err), "recovered file must exist");
    TEST_ASSERT(std::string(bytes.begin(), bytes.end()) == "ORIGINAL",
                "CLI recover must restore original bytes");
    TEST_ASSERT(!fs::exists(stage, ec), "recovered staging dir must be removed");
    fs::remove_all(dir, ec);
    return true;
}

int main() {
    std::cout << "========================================\n"
              << "       bzconv Test Suite\n"
              << "========================================\n";

    RUN_TEST(test_probe_valid_xsi);
    RUN_TEST(test_probe_invalid_file);
    RUN_TEST(test_parse_xsi_geometry);
    RUN_TEST(test_export_glb);
    RUN_TEST(test_export_obj);
    RUN_TEST(test_export_fbx_if_blender_available);
    RUN_TEST(test_animation_only_xsi);
    RUN_TEST(test_real_docb_msh_if_available);
    RUN_TEST(test_atomic_write_file_safety);
    RUN_TEST(test_skin_influence_cap);
    RUN_TEST(test_skin_payload_preserves_8_influences);
    RUN_TEST(test_overwrite_policy);
    RUN_TEST(test_fbx_validation_no_false_pass);
    RUN_TEST(test_result_classification);
    RUN_TEST(test_animation_only_auto_needs_companion);
    RUN_TEST(test_companion_node_index_remap);
    RUN_TEST(test_source_level_p0_guards);
    RUN_TEST(test_glb_exact_influence_limit_and_renormalization);
    RUN_TEST(test_glb_influence_edge_cases);
    RUN_TEST(test_status_glb_loss_forces_partial);
    RUN_TEST(test_gltf_sidecar_overwrite_policy);
    RUN_TEST(test_gltf_texture_sidecar_overwrite_policy);
    RUN_TEST(test_obj_sidecar_overwrite_policy);
    RUN_TEST(test_companion_multiclip_same_bone);
    RUN_TEST(test_output_bundle_cancel_reason);
    RUN_TEST(test_output_bundle_commit_cleans_durable_stage);
    RUN_TEST(test_output_bundle_recover_interrupted_commit);
    RUN_TEST(test_cli_recover_dispatch);
    RUN_TEST(test_glb_model_space_no_double_transform);
    RUN_TEST(test_input_output_identity_guard);
    RUN_TEST(test_glb_texture_loss_diagnostics);
    RUN_TEST(test_glb_rotated_model_space_normal);
    RUN_TEST(test_glb_parent_after_child_order);
    RUN_TEST(test_glb_skinned_model_space_bind_pose);
    RUN_TEST(test_glb_invalid_images_rejected);

    if (bztests_t9::run_all() != 0) g_tests_failed += 1;
    if (bztests_t13::run_all() != 0) g_tests_failed += 1;

    std::cout << "========================================\n"
              << "Summary: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)\n";
        return 1;
    }
    std::cout << " (ALL TESTS PASSED)\n";
    return 0;
}
