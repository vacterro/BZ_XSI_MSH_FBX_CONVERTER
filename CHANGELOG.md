# Changelog

## 1.2.0

- C++20 XSI/MSH conversion core, GLB/glTF/OBJ exporters, Blender FBX bridge,
  Win32 Golden Default GUI.
- T-13: crash-consistent OutputBundle journal with per-file write-ahead
  checkpoints, unique transaction IDs, SHA-256 identity proof and fail-closed
  recovery.
- T-13: source fidelity checker with oriented winding, scale-aware tolerance,
  material/animation/world-matrix checks and feature-level verdicts.
- T-14: material binding identity and full per-primitive assignments verified;
  every animation keyframe checked; texture channel loss detected; source
  manifest records double-sided material property.
