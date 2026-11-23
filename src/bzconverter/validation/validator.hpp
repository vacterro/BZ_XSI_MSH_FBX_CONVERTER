#pragma once
#include "bzconverter/exporters/exporter.hpp"
#include "bzconverter/materials/material_resolver.hpp"
#include <atomic>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace bz {

// Parse either XSI or MSH using content-based Format Probe (never blind extension trust)
bool load_model_file(const std::filesystem::path& input_path, Scene& out_scene, ProbeResult& out_probe,
                     std::string& out_error, bool include_animations = true);

// Map animation-source node indices to companion scene indices by normalized
// name with hierarchy/bind-pose disambiguation. Returns false on ambiguous or
// incomplete coverage; the caller MUST refuse the companion.
bool build_companion_node_map(const Scene& anim_scene, const Scene& base_scene,
                              std::unordered_map<int, int>& mapping, std::string& reason);

// Validate internal invariants of a parsed Canonical Scene
void validate_canonical_scene(Scene& scene);

// Centralized result adjudication. Diagnostic codes with a fidelity-loss
// policy move a successful export to PARTIAL; log text is never consulted.
void adjudicate_result(ExportResult& res, const ExportOptions& opts, const SceneStats& stats);

// Full conversion pipeline: Probe -> Parse -> Validate -> MaterialResolver -> Export -> OutputValidate -> Adjudicate
ExportResult convert_single_file(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 const ExportOptions& opts,
                                 MaterialResolver& resolver,
                                 std::atomic<bool>* cancel_flag = nullptr);

// Build a structured machine-readable JSON inspection report for a Scene + Probe
std::string build_inspect_json(const ProbeResult& probe, const Scene* scene, const std::string& parse_error);

} // namespace bz
