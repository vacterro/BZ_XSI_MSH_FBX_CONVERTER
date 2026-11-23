#include "bzconverter/core/types.hpp"
#include "bzconverter/core/output_bundle.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/formats/probe.hpp"
#include "bzconverter/scene/scene.hpp"
#include "bzconverter/exporters/exporter.hpp"
#include "bzconverter/materials/material_resolver.hpp"
#include "bzconverter/validation/validator.hpp"

#include <iostream>
#include <iomanip>
#include <map>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifndef BZCONV_VERSION
#define BZCONV_VERSION "1.2.0"
#endif
const char* VERSION_STRING = BZCONV_VERSION;

void print_version() {
    std::cout << "bzconv " << VERSION_STRING << " (Battlezone XSI/MSH Model Converter)\n";
}

void print_usage() {
    std::cout << "bzconv " << VERSION_STRING << " - Battlezone XSI/MSH Model Converter\n\n"
              << "Usage:\n"
              << "  bzconv <command> [options] <arguments...>\n"
              << "  bzconv <input-file> [options]                   (Shorthand for convert)\n\n"
              << "Commands:\n"
              << "  probe   <file>                 Probe file format without full parse\n"
              << "  inspect <file>                 Parse file and display detailed scene statistics\n"
              << "  manifest <file>                Dump the parsed canonical scene as a JSON manifest\n"
              << "  recover <dir>                  Roll back interrupted bundles from durable journals\n"
              << "  convert <input> [options]      Convert model file or directory\n"
              << "  version                        Show version information\n"
              << "  help                           Show this help message\n\n"
              << "Convert Options:\n"
              << "  -o, --output <path>            Output file or directory path\n"
              << "  -f, --format <format>          Target format: fbx (default), glb, gltf, obj\n"
              << "  -p, --profile <profile>        Profile: full-scene (default), standard,\n"
              << "                                 geom-only, geom-mat, blender, glb-portable\n"
              << "  -r, --recursive                Process directories recursively\n"
              << "  --no-materials                 Disable material export\n"
              << "  --no-textures                  Disable texture resolution and export\n"
              << "  --no-animations                Disable animation track export\n"
              << "  --flat                         Flatten node hierarchy\n"
              << "  --no-embed                     Do not embed textures in glTF/GLB\n"
              << "  --bzcc-axis                    Export with BZCC axes (Forward=-Y, Up=Z)\n"
              << "  --no-validate                  Skip post-export re-import validation\n"
              << "  --overwrite                    Allow replacing existing output files (off by default)\n"
              << "  --max-influences <N>           Exact GLB skin influence limit (0 = unlimited)\n"
              << "  --strict                       Exit with code 2 on partial warnings\n"
              << "  --game-dir <path>              Battlezone game assets directory\n"
              << "  --search-dir <path>            Extra directory to search for textures/materials\n"
              << "  --workshop-dir <path>          Steam Workshop directory\n"
              << "  --blender <path>               Explicit path to blender.exe\n"
              << "  --bridge-script <path>         Explicit path to bz_fbx_bridge.py\n"
              << "  --json                         Output machine-readable JSON results\n"
              << "  --quiet                        Minimal console output\n\n"
              << "Examples:\n"
              << "  bzconv ivtank00.xsi -o ivtank00.glb\n"
              << "  bzconv convert sgbzka_stand.xsi -f fbx --bzcc-axis\n"
              << "  bzconv inspect ivtank00.xsi --json\n"
              << "  bzconv probe model.msh\n"
              << "  bzconv convert ./xsi_models/ -o ./converted_glb/ -f glb -r\n";
}

struct CliOptions {
    std::string command;
    std::vector<std::string> inputs;
    std::string output_path;
    bz::ExportFormat format = bz::ExportFormat::FBX;
    bool format_explicit = false;
    bz::ConversionProfile profile = bz::ConversionProfile::FullScene;
    bool recursive = false;
    bool include_materials = true;
    bool include_textures = true;
    bool include_animations = true;
    bool preserve_hierarchy = true;
    bool embed_textures = true;
    bool bzcc_axis_mode = false;
    bool validate_output = true;
    bool overwrite = false;
    size_t glb_max_influences = 0;
    bool strict = false;
    bool json_output = false;
    bool quiet = false;
    std::vector<std::string> game_dirs;
    std::vector<std::string> search_dirs;
    std::string workshop_dir;
    std::string companion;
    std::string blender_exe;
    std::string bridge_script;
};

bool parse_args(const std::vector<std::string>& args, CliOptions& opt) {
    if (args.empty()) {
        opt.command = "help";
        return true;
    }

    size_t i = 0;
    std::string first = args[0];
    if (first == "help" || first == "--help" || first == "-h" || first == "/?") {
        opt.command = "help";
        return true;
    }
    if (first == "version" || first == "--version" || first == "-v") {
        opt.command = "version";
        return true;
    }

    if (first == "probe" || first == "inspect" || first == "convert" || first == "manifest" || first == "recover") {
        opt.command = first;
        i = 1;
    } else if (!first.empty() && first[0] != '-') {
        // Direct shorthand: bzconv input.xsi -o output.glb
        opt.command = "convert";
        opt.inputs.push_back(first);
        i = 1;
    } else {
        opt.command = "convert";
    }

    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help") {
            opt.command = "help";
            return true;
        } else if (a == "-v" || a == "--version") {
            opt.command = "version";
            return true;
        } else if (a == "-o" || a == "--output") {
            if (i + 1 < args.size()) opt.output_path = args[++i];
        } else if (a == "-f" || a == "--format") {
            if (i + 1 < args.size()) {
                std::string fstr = bz::to_lower(bz::trim(args[++i]));
                opt.format = bz::export_format_from_string(fstr);
                opt.format_explicit = true;
            }
        } else if (a == "-p" || a == "--profile") {
            if (i + 1 < args.size()) {
                std::string pstr = bz::to_lower(bz::trim(args[++i]));
                if (pstr == "standard" || pstr == "standardfbx") opt.profile = bz::ConversionProfile::StandardFBX;
                else if (pstr == "geom-only" || pstr == "geometryonly") opt.profile = bz::ConversionProfile::GeometryOnly;
                else if (pstr == "geom-mat" || pstr == "geometrymaterials") opt.profile = bz::ConversionProfile::GeometryMaterials;
                else if (pstr == "blender" || pstr == "blendercompat") opt.profile = bz::ConversionProfile::BlenderCompat;
                else if (pstr == "glb-portable" || pstr == "glbportable") opt.profile = bz::ConversionProfile::GLBPortable;
                else opt.profile = bz::ConversionProfile::FullScene;
            }
        } else if (a == "-r" || a == "--recursive") {
            opt.recursive = true;
        } else if (a == "--no-materials") {
            opt.include_materials = false;
        } else if (a == "--no-textures") {
            opt.include_textures = false;
        } else if (a == "--no-animations") {
            opt.include_animations = false;
        } else if (a == "--flat") {
            opt.preserve_hierarchy = false;
        } else if (a == "--no-embed") {
            opt.embed_textures = false;
        } else if (a == "--bzcc-axis") {
            opt.bzcc_axis_mode = true;
        } else if (a == "--no-validate") {
            opt.validate_output = false;
        } else if (a == "--overwrite") {
            opt.overwrite = true;
        } else if (a == "--max-influences" && i + 1 < args.size()) {
            opt.glb_max_influences = static_cast<size_t>(std::stoul(args[++i]));
        } else if (a == "--strict") {
            opt.strict = true;
        } else if (a == "--json") {
            opt.json_output = true;
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "--game-dir") {
            if (i + 1 < args.size()) opt.game_dirs.push_back(args[++i]);
        } else if (a == "--search-dir") {
            if (i + 1 < args.size()) opt.search_dirs.push_back(args[++i]);
        } else if (a == "--workshop-dir") {
            if (i + 1 < args.size()) opt.workshop_dir = args[++i];
        } else if (a == "--companion") {
            if (i + 1 < args.size()) opt.companion = args[++i];
        } else if (a == "--blender") {
            if (i + 1 < args.size()) opt.blender_exe = args[++i];
        } else if (a == "--bridge-script") {
            if (i + 1 < args.size()) opt.bridge_script = args[++i];
        } else if (!a.empty() && a[0] != '-') {
            opt.inputs.push_back(a);
        } else {
            std::cerr << "Warning: unrecognized option: " << a << "\n";
        }
    }

    return true;
}

int handle_probe(const CliOptions& opt) {
    if (opt.inputs.empty()) {
        std::cerr << "Error: 'probe' requires an input file path.\n";
        return 1;
    }

    fs::path p = bz::path_from_utf8(opt.inputs[0]);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::cerr << "Error: file does not exist: " << opt.inputs[0] << "\n";
        return 1;
    }

    bz::ProbeResult pr = bz::probe_file(p);

    if (opt.json_output) {
        bz::Json j = bz::Json::object();
        j["file"] = opt.inputs[0];
        j["format"] = bz::format_to_string(pr.format);
        j["supported"] = pr.supported;
        j["magicSummary"] = pr.magic_summary;
        j["detail"] = pr.detail;
        j["majorVersion"] = pr.major_version;
        j["minorVersion"] = pr.minor_version;
        j["fileSize"] = pr.file_size;
        j["sha256"] = pr.sha256;
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << "File:      " << opt.inputs[0] << "\n"
                  << "Format:    " << bz::format_to_string(pr.format) << "\n"
                  << "Supported: " << (pr.supported ? "YES" : "NO") << "\n"
                  << "Details:   " << pr.detail << "\n"
                  << "Magic:     " << pr.magic_summary << "\n"
                  << "Size:      " << pr.file_size << " bytes\n"
                  << "SHA-256:   " << pr.sha256 << "\n";
    }

    return pr.supported ? 0 : 1;
}

// SAIHANDOFF-12 §10: dump the parsed Canonical Scene as a JSON manifest for
// independent source-to-output fidelity verification. The manifest contains
// everything needed to verify an export WITHOUT consulting the exported file:
// world-space triangles, per-corner UVs/normals, hierarchy, materials,
// texture refs, skinning and animation keyframes.
static int handle_manifest(const CliOptions& opt) {
    if (opt.inputs.empty()) {
        std::cerr << "Error: 'manifest' requires an input file path.\n";
        return 1;
    }
    fs::path p = bz::path_from_utf8(opt.inputs[0]);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::cerr << "Error: file does not exist: " << opt.inputs[0] << "\n";
        return 1;
    }

    bz::Scene scene;
    bz::ProbeResult probe;
    std::string err;
    bool ok = bz::load_model_file(p, scene, probe, err, opt.include_animations);
    if (!ok) {
        std::cerr << "Parse Error: " << err << "\n";
        return 1;
    }
    scene.update_world_matrices();

    // Requested profile (T-13): verdicts distinguish requested features from
    // intentionally excluded or lost ones.
    bool req_materials = opt.include_materials;
    bool req_textures = opt.include_textures;
    bool req_animations = opt.include_animations;
    std::string req_profile = bz::profile_to_string(opt.profile);

    auto num = [](float v) { return bz::Json(static_cast<double>(v)); };
    auto vec3 = [&](const bz::Vec3& v) {
        bz::Json a = bz::Json::array();
        a.push_back(num(v.x)); a.push_back(num(v.y)); a.push_back(num(v.z));
        return a;
    };
    auto mat4 = [&](const bz::Mat4& m) {
        bz::Json rows = bz::Json::array();
        for (int r = 0; r < 4; ++r) {
            bz::Json row = bz::Json::array();
            for (int c = 0; c < 4; ++c) row.push_back(num(m.at(r, c)));
            rows.push_back(std::move(row));
        }
        return rows;
    };

    // World-space content per node (column-vector convention, matching glTF:
    // world = parent @ local; Mat4 is row-vector storage, so the glTF matrix
    // is the transpose)
    bz::Json jnodes = bz::Json::array();
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
        const auto& n = scene.nodes[ni];
        bz::Json jn = bz::Json::object();
        jn["name"] = n.name;
        jn["parent"] = n.parent_index;
        jn["is_bone"] = n.is_bone;
        jn["is_hardpoint"] = n.is_hardpoint;
        jn["worldMatrix"] = mat4(n.world_matrix.transposed()); // glTF column-vector
        if (n.mesh_index >= 0) {
            const auto& mesh = scene.meshes[size_t(n.mesh_index)];
            // Mirror the exporter's localization rules EXACTLY (glb_exporter
            // "Model-space coordinates must not be transformed twice"):
            // skinned -> identity; single referencing node -> inverse world;
            // everything else -> identity with the raw positions.
            std::vector<std::vector<int>> mesh_nodes(scene.meshes.size());
            for (size_t wni = 0; wni < scene.nodes.size(); ++wni) {
                int wmi = scene.nodes[wni].mesh_index;
                if (wmi >= 0 && wmi < static_cast<int>(scene.meshes.size()))
                    mesh_nodes[wmi].push_back(static_cast<int>(wni));
            }
            const auto& nodes_ref = mesh_nodes[size_t(n.mesh_index)];
            bz::Mat4 localize = bz::Mat4::identity();
            bool localize_ok = true;
            if (mesh.vertices_in_model_space) {
                if (mesh.is_skinned) {
                    localize_ok = false;
                } else if (nodes_ref.size() == 1 && scene.nodes[size_t(nodes_ref[0])].world_matrix.invert_affine(localize)) {
                    // localized to the single referencing node's space
                } else {
                    localize_ok = false;
                }
            }
            bz::Json jprims = bz::Json::array();
            // group triangles by material — t counts TRIANGLES here, exactly
            // like the glb exporter's mesh_primitive_groups loop
            size_t ntris = mesh.indices.size() / 3;
            struct PrimGroup { bz::Json corners; bz::Json uv; bz::Json nrm; };
            std::map<int, PrimGroup> groups;
            std::vector<int> group_order;
            for (size_t t = 0; t < ntris; ++t) {
                int m = (t < mesh.triangle_materials.size())
                            ? mesh.triangle_materials[t] : -1;
                auto it = groups.find(m);
                if (it == groups.end()) {
                    it = groups.emplace(m, PrimGroup{bz::Json::array(), bz::Json::array(), bz::Json::array()}).first;
                    group_order.push_back(m);
                }
                for (int k = 0; k < 3; ++k) {
                    const auto& v = mesh.vertices[mesh.indices[t * 3 + size_t(k)]];
                    bz::Vec3 p = v.pos;
                    if (mesh.vertices_in_model_space && localize_ok)
                        p = localize.transform_point(p);
                    bz::Json jt = bz::Json::array();
                    jt.push_back(num(p.x)); jt.push_back(num(p.y)); jt.push_back(num(p.z));
                    it->second.corners.push_back(std::move(jt));
                    if (mesh.has_uvs) {
                        bz::Json ju = bz::Json::array();
                        ju.push_back(num(v.uv.x)); ju.push_back(num(v.uv.y));
                        it->second.uv.push_back(std::move(ju));
                    }
                    if (mesh.has_normals) {
                        bz::Json jnn = bz::Json::array();
                        jnn.push_back(num(v.normal.x)); jnn.push_back(num(v.normal.y)); jnn.push_back(num(v.normal.z));
                        it->second.nrm.push_back(std::move(jnn));
                    }
                }
            }
            for (int mkey : group_order) {
                bz::Json jg = bz::Json::object();
                jg["material"] = mkey;
                if (mkey >= 0 && mkey < static_cast<int>(scene.materials.size())) {
                    const auto& smat = scene.materials[size_t(mkey)];
                    jg["materialName"] = smat.name.empty() ? std::string("material") : smat.name;
                    bz::Json texrefs = bz::Json::array();
                    auto add_tex_ref = [&](const std::string& ref) {
                        if (!ref.empty()) texrefs.push_back(ref);
                    };
                    add_tex_ref(smat.diffuse_tex.original_ref);
                    add_tex_ref(smat.normal_tex.original_ref);
                    add_tex_ref(smat.emissive_tex.original_ref);
                    add_tex_ref(smat.specular_tex.original_ref);
                    add_tex_ref(smat.team_color_tex.original_ref);
                    jg["textureRefs"] = std::move(texrefs);
                    jg["doubleSided"] = smat.double_sided;
                }
                jg["corners"] = groups[mkey].corners;  // flat corner list: 3 entries per triangle
                if (mesh.has_uvs) jg["uvCorners"] = groups[mkey].uv;
                if (mesh.has_normals) jg["normalCorners"] = groups[mkey].nrm;
                jprims.push_back(std::move(jg));
            }
            jn["uvOriginBottomLeft"] = scene.uv_origin_bottom_left;
            jn["primitives"] = std::move(jprims);
            jn["vertexCount"] = static_cast<double>(mesh.vertices.size());
            jn["triangleCount"] = static_cast<double>(mesh.indices.size() / 3);
        }
        if (n.is_bone || n.is_hardpoint) {
            jn["flags"] = std::string(n.is_bone ? "bone" : "") +
                          (n.is_hardpoint ? (n.is_bone ? "+hardpoint" : "hardpoint") : "");
        }
        jnodes.push_back(std::move(jn));
    }

    // animation samples: per clip, per track, first/mid/last keyframes
    bz::Json jclips = bz::Json::array();
    for (const auto& clip : scene.animations) {
        bz::Json jc = bz::Json::object();
        jc["name"] = clip.name;
        jc["fps"] = num(clip.fps);
        jc["startFrame"] = num(clip.start_frame);
        jc["endFrame"] = num(clip.end_frame);
        jc["trackCount"] = static_cast<double>(clip.tracks.size());
        bz::Json jtracks = bz::Json::array();
        for (const auto& tr : clip.tracks) {
            bz::Json jt = bz::Json::object();
            jt["nodeIndex"] = tr.node_index;
            jt["nodeName"] = tr.node_name;
            auto vec_keys = [&](const std::vector<bz::KeyframeVec3>& keys) {
                bz::Json arr = bz::Json::array();
                for (const auto& k : keys) {
                    bz::Json kk = bz::Json::array();
                    kk.push_back(num(k.frame));
                    kk.push_back(num(k.value.x));
                    kk.push_back(num(k.value.y));
                    kk.push_back(num(k.value.z));
                    arr.push_back(std::move(kk));
                }
                return arr;
            };
            jt["translations"] = vec_keys(tr.translations);
            jt["scales"] = vec_keys(tr.scales);
            bz::Json rots = bz::Json::array();
            for (const auto& k : tr.rotations) {
                bz::Json kk = bz::Json::array();
                kk.push_back(num(k.frame));
                kk.push_back(num(k.value.w));
                kk.push_back(num(k.value.x));
                kk.push_back(num(k.value.y));
                kk.push_back(num(k.value.z));
                rots.push_back(std::move(kk));
            }
            jt["rotations"] = std::move(rots);
            jtracks.push_back(std::move(jt));
        }
        jc["tracks"] = std::move(jtracks);
        jclips.push_back(std::move(jc));
    }

    bz::Json j = bz::Json::object();
    j["format"] = "bzconv-source-manifest";
    j["version"] = 2;
    j["requestedProfile"] = req_profile;
    j["requestedFeatures"] = bz::Json::object();
    j["requestedFeatures"]["materials"] = req_materials;
    j["requestedFeatures"]["textures"] = req_textures;
    j["requestedFeatures"]["animations"] = req_animations;
    bz::Json jmaterials = bz::Json::array();
    for (const auto& smat : scene.materials) {
        bz::Json jm = bz::Json::object();
        jm["name"] = smat.name.empty() ? std::string("material") : smat.name;
        jm["materialFileRef"] = smat.material_file_ref;
        jm["doubleSided"] = smat.double_sided;
        bz::Json jtex = bz::Json::object();
        auto tex_info = [&](const bz::TextureChannel& ch) {
            bz::Json t = bz::Json::object();
            t["ref"] = ch.original_ref;
            t["resolvedPath"] = ch.resolved_path;
            t["srgb"] = ch.is_srgb;
            return t;
        };
        jtex["diffuse"] = tex_info(smat.diffuse_tex);
        jtex["normal"] = tex_info(smat.normal_tex);
        jtex["emissive"] = tex_info(smat.emissive_tex);
        jtex["specular"] = tex_info(smat.specular_tex);
        jtex["teamColor"] = tex_info(smat.team_color_tex);
        jm["textures"] = std::move(jtex);
        jmaterials.push_back(std::move(jm));
    }
    j["materials"] = std::move(jmaterials);
    j["sourcePath"] = scene.source_path;
    j["sourceFormat"] = bz::format_to_string(scene.source_format);
    j["sourceSha256"] = scene.source_sha256;
    j["nodes"] = std::move(jnodes);
    j["animations"] = std::move(jclips);

    fs::path out = opt.output_path.empty()
                       ? p.parent_path() / (p.stem().wstring() + L"_manifest.json")
                       : bz::path_from_utf8(opt.output_path);
    std::string text = j.dump(2);
    std::string werr;
    if (!bz::write_file_atomic(out, text.data(), text.size(), werr)) {
        std::cerr << "Error: cannot write manifest: " << werr << "\n";
        return 1;
    }
    std::cout << "Manifest: " << bz::utf8_from_path(out) << " ("
              << scene.nodes.size() << " nodes, " << scene.meshes.size()
              << " meshes, " << scene.animations.size() << " clips)\n";
    return 0;
}

int handle_inspect(const CliOptions& opt) {
    if (opt.inputs.empty()) {
        std::cerr << "Error: 'inspect' requires an input file path.\n";
        return 1;
    }

    fs::path p = bz::path_from_utf8(opt.inputs[0]);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::cerr << "Error: file does not exist: " << opt.inputs[0] << "\n";
        return 1;
    }

    bz::Scene scene;
    bz::ProbeResult probe;
    std::string err;
    bool ok = bz::load_model_file(p, scene, probe, err, opt.include_animations);

    if (opt.json_output) {
        std::cout << bz::build_inspect_json(probe, ok ? &scene : nullptr, err) << "\n";
        return ok ? 0 : 1;
    }

    std::cout << "Inspection: " << opt.inputs[0] << "\n"
              << "Format:     " << bz::format_to_string(probe.format)
              << " (supported: " << (probe.supported ? "YES" : "NO") << ")\n"
              << "File Size:  " << probe.file_size << " bytes\n"
              << "SHA-256:    " << probe.sha256 << "\n\n";

    if (!ok) {
        std::cerr << "Parse Error: " << err << "\n";
        return 1;
    }

    auto st = scene.compute_stats();
    std::cout << "--- Scene Statistics ---\n"
              << "  Nodes:              " << st.node_count << "\n"
              << "  Meshes:             " << st.mesh_count << " (skinned: " << st.skinned_mesh_count << ")\n"
              << "  Bones:              " << st.bone_count << "\n"
              << "  Hardpoints:         " << st.hardpoint_count << "\n"
              << "  Total Triangles:    " << st.total_triangles << "\n"
              << "  Total Vertices:     " << st.total_vertices << "\n"
              << "  Source Polygons:    " << st.source_polygons << "\n"
              << "  Materials:          " << st.material_count << "\n"
              << "  Animation Clips:    " << st.animation_clip_count << "\n"
              << "  Animation Tracks:   " << st.animation_track_count << "\n"
              << "  Has Normals:        " << (st.has_normals ? "YES" : "NO") << "\n"
              << "  Has UVs:            " << (st.has_uvs ? "YES" : "NO") << "\n"
              << "  Has Vertex Colors:  " << (st.has_vertex_colors ? "YES" : "NO") << "\n"
              << "  Normal Agreement:   " << int(st.normal_face_agreement * 100) << "%\n"
              << "  Degenerate Tris:    " << st.degenerate_triangles << "\n"
              << "  Negative Det Nodes: " << st.negative_det_nodes << "\n"
              << "  Bounding Box Min:   [" << st.world_bbox.lo.x << ", " << st.world_bbox.lo.y << ", " << st.world_bbox.lo.z << "]\n"
              << "  Bounding Box Max:   [" << st.world_bbox.hi.x << ", " << st.world_bbox.hi.y << ", " << st.world_bbox.hi.z << "]\n\n";

    if (!scene.materials.empty()) {
        std::cout << "--- Materials (" << scene.materials.size() << ") ---\n";
        for (size_t mi = 0; mi < scene.materials.size(); ++mi) {
            const auto& m = scene.materials[mi];
            std::cout << "  [" << mi << "] " << m.name;
            if (!m.material_file_ref.empty()) std::cout << " (mat file: " << m.material_file_ref << ")";
            if (!m.diffuse_tex.original_ref.empty()) std::cout << " [diffuse: " << m.diffuse_tex.original_ref << "]";
            std::cout << "\n";
        }
        std::cout << "\n";
    }

    if (!scene.animations.empty()) {
        std::cout << "--- Animations (" << scene.animations.size() << ") ---\n";
        for (const auto& a : scene.animations) {
            std::cout << "  Clip: " << a.name << " (frames: " << a.start_frame << ".." << a.end_frame
                      << ", fps: " << a.fps << ", tracks: " << a.tracks.size() << ")\n";
        }
        std::cout << "\n";
    }

    if (!scene.diagnostics.empty()) {
        std::cout << "--- Diagnostics (" << scene.diagnostics.size() << ") ---\n";
        for (const auto& d : scene.diagnostics) {
            const char* sev = (d.severity == bz::DiagSeverity::Error) ? "ERROR"
                            : (d.severity == bz::DiagSeverity::Warning ? "WARN" : "INFO");
            std::cout << "  [" << sev << "] " << d.code << ": " << d.message << "\n";
        }
    }

    return 0;
}

fs::path deduce_output_path(const fs::path& input_path, const std::string& explicit_out,
                            bz::ExportFormat format, bool is_batch, const fs::path& base_in_dir) {
    std::string ext = "." + std::string(bz::export_format_to_string(format));
    if (!explicit_out.empty()) {
        fs::path out_p = bz::path_from_utf8(explicit_out);
        if (is_batch) {
            // Keep relative sub-path if recursing
            std::error_code ec;
            fs::path rel = fs::relative(input_path, base_in_dir, ec);
            if (ec || rel.empty()) rel = input_path.filename();
            rel.replace_extension(ext);
            return out_p / rel;
        } else {
            // Single file target: if explicit_out is an existing directory or ends with slash, put inside
            if (fs::is_directory(out_p) || explicit_out.back() == '/' || explicit_out.back() == '\\') {
                fs::path fn = input_path.stem();
                fn += ext;
                return out_p / fn;
            }
            return out_p;
        }
    }

    // Default: write next to input file
    fs::path def = input_path;
    def.replace_extension(ext);
    return def;
}

int handle_convert(const CliOptions& opt) {
    if (opt.inputs.empty()) {
        std::cerr << "Error: 'convert' requires at least one input file or directory.\n";
        return 1;
    }

    bz::ResolverConfig rcfg;
    for (const auto& d : opt.game_dirs) rcfg.game_asset_roots.push_back(bz::path_from_utf8(d));
    for (const auto& d : opt.search_dirs) rcfg.user_search_roots.push_back(bz::path_from_utf8(d));
    if (!opt.workshop_dir.empty()) {
        rcfg.workshop_root = bz::path_from_utf8(opt.workshop_dir);
        rcfg.search_workshop = true;
    }
    // Auto-detect Battlezone game directories if user didn't specify any
    if (rcfg.game_asset_roots.empty()) {
        rcfg.game_asset_roots = bz::MaterialResolver::detect_default_game_roots();
    }
    if (rcfg.workshop_root.empty()) {
        rcfg.workshop_root = bz::MaterialResolver::detect_default_workshop_root();
        if (!rcfg.workshop_root.empty()) rcfg.search_workshop = true;
    }

    bz::MaterialResolver resolver(rcfg);

    bz::ExportOptions exopts;
    exopts.format = opt.format;
    exopts.profile = opt.profile;
    exopts.include_materials = opt.include_materials;
    exopts.include_textures = opt.include_textures;
    exopts.include_animations = opt.include_animations;
    exopts.preserve_hierarchy = opt.preserve_hierarchy;
    exopts.embed_textures = opt.embed_textures;
    exopts.bzcc_axis_mode = opt.bzcc_axis_mode;
    exopts.validate_output = opt.validate_output;
    exopts.overwrite = opt.overwrite;
    if (!opt.companion.empty()) exopts.companion_model = bz::path_from_utf8(opt.companion);
    exopts.glb_max_influences = opt.glb_max_influences ? opt.glb_max_influences
        : ((opt.profile == bz::ConversionProfile::GLBPortable) ? 4 : 0);
    if (!opt.blender_exe.empty()) exopts.blender_exe = bz::path_from_utf8(opt.blender_exe);
    if (!opt.bridge_script.empty()) exopts.bridge_script = bz::path_from_utf8(opt.bridge_script);

    // Collect all input files
    struct TaskItem {
        fs::path in_file;
        fs::path base_dir;
    };
    std::vector<TaskItem> tasks;

    for (const auto& inp : opt.inputs) {
        fs::path p = bz::path_from_utf8(inp);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            std::cerr << "Error: path not found: " << inp << "\n";
            return 1;
        }
        if (fs::is_directory(p, ec)) {
            if (opt.recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(p, ec)) {
                    if (entry.is_regular_file(ec)) {
                        std::string ext = bz::to_lower(entry.path().extension().string());
                        if (ext == ".xsi" || ext == ".msh") {
                            tasks.push_back({entry.path(), p});
                        }
                    }
                }
            } else {
                for (const auto& entry : fs::directory_iterator(p, ec)) {
                    if (entry.is_regular_file(ec)) {
                        std::string ext = bz::to_lower(entry.path().extension().string());
                        if (ext == ".xsi" || ext == ".msh") {
                            tasks.push_back({entry.path(), p});
                        }
                    }
                }
            }
        } else {
            tasks.push_back({p, p.parent_path()});
        }
    }

    if (tasks.empty()) {
        std::cerr << "Error: no valid .xsi or .msh input files found.\n";
        return 1;
    }

    bool is_batch = (tasks.size() > 1);

    // If single file and output was explicitly specified with extension, infer format from output extension unless -f was given
    if (!is_batch && !opt.output_path.empty() && !opt.format_explicit) {
        std::string out_ext = bz::to_lower(fs::path(opt.output_path).extension().string());
        if (!out_ext.empty() && out_ext[0] == '.') out_ext = out_ext.substr(1);
        if (out_ext == "glb") exopts.format = bz::ExportFormat::GLB;
        else if (out_ext == "gltf") exopts.format = bz::ExportFormat::GLTF;
        else if (out_ext == "obj") exopts.format = bz::ExportFormat::OBJ;
        else if (out_ext == "fbx") exopts.format = bz::ExportFormat::FBX;
    }

    if (!opt.quiet && !opt.json_output) {
        std::cout << "Converting " << tasks.size() << " model(s) to "
                  << bz::export_format_to_string(exopts.format) << "...\n";
    }

    size_t count_ok = 0;
    size_t count_partial = 0;
    size_t count_failed = 0;
    std::vector<bz::ExportResult> all_results;
    std::vector<std::string> task_in_paths;

    auto t_start = std::chrono::steady_clock::now();

    for (size_t ti = 0; ti < tasks.size(); ++ti) {
        const auto& item = tasks[ti];
        fs::path out_path = deduce_output_path(item.in_file, opt.output_path, exopts.format, is_batch, item.base_dir);

        if (!opt.quiet && !opt.json_output && is_batch) {
            std::cout << "[" << (ti + 1) << "/" << tasks.size() << "] "
                      << bz::utf8_from_path(item.in_file.filename()) << " -> " << bz::utf8_from_path(out_path.filename()) << "... "
                      << std::flush;
        }

        bz::ExportResult res = bz::convert_single_file(item.in_file, out_path, exopts, resolver);
        all_results.push_back(res);
        task_in_paths.push_back(bz::utf8_from_path(item.in_file));

        if (res.status == bz::JobStatus::Success) {
            ++count_ok;
            if (!opt.quiet && !opt.json_output) {
                if (is_batch) std::cout << "OK (" << std::fixed << std::setprecision(1) << res.elapsed_ms << "ms)\n";
                else std::cout << "SUCCESS: " << res.summary << " in " << std::fixed << std::setprecision(1) << res.elapsed_ms << "ms\n"
                               << "Output: " << res.output_path << "\n";
            }
        } else if (res.status == bz::JobStatus::Partial) {
            ++count_partial;
            if (!opt.quiet && !opt.json_output) {
                if (is_batch) std::cout << "PARTIAL (" << res.summary << ")\n";
                else std::cout << "PARTIAL: " << res.summary << " in " << std::fixed << std::setprecision(1) << res.elapsed_ms << "ms\n"
                               << "Output: " << res.output_path << "\n";
            }
        } else {
            ++count_failed;
            if (!opt.quiet && !opt.json_output) {
                if (is_batch) std::cout << "FAILED (" << res.summary << ")\n";
                else std::cout << "FAILED: " << res.summary << "\n";
            }
        }

        // Print diagnostics for single-file mode
        if (!is_batch && !opt.quiet && !opt.json_output && !res.diagnostics.empty()) {
            std::cout << "--- Diagnostics ---\n";
            for (const auto& d : res.diagnostics) {
                const char* sev = (d.severity == bz::DiagSeverity::Error) ? "ERROR"
                                : (d.severity == bz::DiagSeverity::Warning ? "WARN" : "INFO");
                std::cout << "  [" << sev << "] " << d.code << ": " << d.message << "\n";
            }
        }
    }

    auto total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();

    if (opt.json_output) {
        bz::Json root = bz::Json::object();
        root["total"] = tasks.size();
        root["succeeded"] = count_ok;
        root["partial"] = count_partial;
        root["failed"] = count_failed;
        root["totalElapsedMs"] = total_ms;
        bz::Json jitems = bz::Json::array();
        for (size_t i = 0; i < all_results.size(); ++i) {
            const auto& r = all_results[i];
            bz::Json ji = bz::Json::object();
            ji["input"] = task_in_paths[i];
            ji["output"] = r.output_path;
            ji["status"] = bz::status_to_string(r.status);
            ji["resultClass"] = bz::result_class_to_string(r.result_class);
            ji["summary"] = r.summary;
            bz::Json jfv = bz::Json::object();
            jfv["geometry"] = r.validation.geometry_ok;
            jfv["normals"] = r.validation.normals_ok;
            jfv["uvs"] = r.validation.uvs_ok;
            jfv["hierarchy"] = r.validation.hierarchy_ok;
            jfv["materials"] = r.validation.materials_ok;
            jfv["skeleton"] = r.validation.skeleton_ok;
            jfv["animations"] = r.validation.animations_ok;
            jfv["weightAgreement"] = r.validation.weight_agreement;
            jfv["weightSumError"] = r.validation.weight_sum_error;
            jfv["maxInfluencesSrc"] = r.validation.src_max_influences;
            jfv["maxInfluencesDst"] = r.validation.dst_max_influences;
            jfv["hierarchyAgreement"] = r.validation.hierarchy_agreement;
            ji["featureFidelity"] = std::move(jfv);
            ji["elapsedMs"] = r.elapsed_ms;
            ji["validationPassed"] = r.validation.passed;
            bz::Json jdiags = bz::Json::array();
            for (const auto& d : r.diagnostics) {
                bz::Json jd = bz::Json::object();
                jd["severity"] = (d.severity == bz::DiagSeverity::Error) ? "ERROR"
                               : (d.severity == bz::DiagSeverity::Warning ? "WARN" : "INFO");
                jd["code"] = d.code;
                jd["message"] = d.message;
                jdiags.push_back(std::move(jd));
            }
            ji["diagnostics"] = std::move(jdiags);
            jitems.push_back(std::move(ji));
        }
        root["results"] = std::move(jitems);
        std::cout << root.dump(2) << "\n";
    } else if (is_batch && !opt.quiet) {
        std::cout << "\nBatch Finished in " << std::fixed << std::setprecision(1) << (total_ms / 1000.0) << "s: "
                  << count_ok << " OK, " << count_partial << " PARTIAL, " << count_failed << " FAILED.\n";
    }

    if (count_failed > 0) return 1;
    if (opt.strict && count_partial > 0) return 2;
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                std::string s(n, '\0');
                int written = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
                if (written > 0) {
                    s.pop_back(); // Remove terminating NUL
                    args.push_back(std::move(s));
                }
            } else if (n == 1) {
                args.push_back({});
            }
        }
        LocalFree(wargv);
    } else
#endif
    {
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
    }

    CliOptions opt;
    if (!parse_args(args, opt)) {
        return 1;
    }

    if (opt.command == "version") {
        print_version();
        return 0;
    }
    if (opt.command == "help") {
        print_usage();
        return 0;
    }
    if (opt.command == "probe") {
        return handle_probe(opt);
    }
    if (opt.command == "manifest") {
        return handle_manifest(opt);
    }
    if (opt.command == "inspect") {
        return handle_inspect(opt);
    }
    if (opt.command == "recover") {
        if (opt.inputs.empty()) {
            std::cerr << "Error: 'recover' requires a directory path.\n";
            return 1;
        }
        std::string report;
        bool ok = bz::OutputBundle::recover_pending(bz::path_from_utf8(opt.inputs[0]), report);
        if (!report.empty()) std::cout << report << "\n";
        std::cout << (ok ? "RECOVERY: OK\n" : "RECOVERY: FAILED\n");
        return ok ? 0 : 1;
    }


    if (opt.command == "convert") {
        return handle_convert(opt);
    }

    std::cerr << "Unknown command: " << opt.command << ". Run 'bzconv --help' for usage.\n";
    return 1;
}
