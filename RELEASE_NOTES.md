# bzconv 0.0.1

P0 fidelity repair over 1.1.0:
- GLB: multiple JOINTS_n/WEIGHTS_n sets (1/4/5/8 influences), zero-weight vertices stay unassigned, explicit influence-limit diagnostics for compatibility profile.
- Companion workflow: animation tracks remapped from animation-only node indices to companion node indices with ambiguity and coverage checks.
- FBX oracle: observed-only hierarchy signature with documented normalization, vertex-index keyed weight and UV comparison, five-frame animation sampling.
- GUI: output paths recomputed from current format/destination before conversion, preflight (missing input, extension, collisions, overwrite), LB_GETTEXTLEN-safe queue painter, multiselect Remove, DPI-aware compact owner-draw Golden Default UI.
- Portable core: non-Windows string-mix compile defect fixed.

P0 data-integrity repair (this revision):
- GLB influence limit is now exact: duplicate joints merged, deterministic ordering, cap applied before normalization, and only kept weights renormalized. Verified directly from GLB WEIGHTS_n accessors for limits 0..8 against source counts 1,4,5,8,12.
- Diagnostic result policy is centralized. GLB_INFLUENCE_LIMIT_LOSS moves the result to PARTIAL; status no longer depends on log wording.
- All multi-file outputs use one transactional bundle: GLTF .bin and textures, OBJ .mtl and textures are declared and preflighted before any write. overwrite=false preserves every existing user file. Failed commits restore originals.
- Companion coverage counts unique animated source nodes, so multiple clips animating the same bone map correctly while missing or ambiguous targets still fail.
- GUI preflight now checks GLTF .bin, OBJ .mtl and existing texture sidecars. Embedded NUL in a wide literal replaced with L'\0'.
- Version metadata now comes from the VERSION file; CLI and glTF generator report 0.0.1.

Data-safety repair (SAIHANDOFF-9):
- GLB no longer double-transforms `vertices_in_model_space` geometry. Positions/normals are localized through the inverse node world matrix; non-invertible transforms get an explicit identity node and diagnostic.
- Core conversion refuses input/output path identity using canonical, case-insensitive and filesystem-equivalence checks, even with `--overwrite`.
- GLB texture channels report missing/invalid files with stable diagnostics and PARTIAL result; PNG/JPEG get structural validation, not just magic-byte checks.
- OutputBundle cancellation keeps the primary reason (`CANCELLED_BEFORE_COMMIT` / `CANCELLED_DURING_COMMIT`) separate from rollback status.
- GUI no longer blocks conversions because of unrelated files in a `_textures` directory.

Transform and texture integrity repair (SAIHANDOFF-10):
- Model-space normals now use the correct world-to-local matrix; rotated-normal regression verifies reconstructed direction.
- World matrices are resolved recursively, independent of node storage order, with cycle and invalid-parent diagnostics.
- Skinned model-space meshes keep bind coordinates under an identity node; skeleton root is only set from an unambiguous bone root.
- PNG textures get full zlib decode validation: chunk CRC, IHDR bounds, IDAT inflate and filter bytes. JPEG gets strict marker-structure validation. Invalid images emit `GLTF_TEXTURE_INVALID` and PARTIAL.


T-13 data-safety and verification repair (this revision):
- OutputBundle v2 write-ahead journal: durable write-intent/written checkpoint before and after every file write, unique transaction IDs, original/staged SHA-256 identity proof, conservative full rollback, idempotent restart recovery, evidence preserved on failed rollback or third-party modification. prepare() refuses to touch an unresolved transaction directory, so recovery backups can no longer be clobbered.
- Source manifest v2: material names, texture refs, per-primitive material bindings and full animation keyframe values (translations, rotations as w,x,y,z, scales) with requested profile/feature flags.
- Source fidelity: per-primitive material binding comparison, animation clip/track/keyframe verification, oriented triangle winding with determinant-aware handedness, effective world matrix check for explicit glTF matrix nodes, scale-aware position tolerance. Verdicts now distinguish SOURCE_FIDELITY_VERIFIED, GEOMETRY_FIDELITY_VERIFIED and SOURCE_FIDELITY_PARTIAL; geometry-only exports never claim full-scene fidelity.
- glTF evaluator: accessor decoding enforces owning bufferView.byteLength/byteOffset, stride, alignment and buffer bounds; skin validation sums all JOINTS_n/WEIGHTS_n sets and checks continuity, counts, ranges and normalization.
- New RED/GREEN suites: 5 C++ crash-recovery witnesses, 8 source-fidelity witnesses, 3 evaluator witnesses. All GREEN after repair; real fvtank XSI and ivtank00 MSH GLB SOURCE_FIDELITY_VERIFIED on fresh conversions.

FBX export still requires Blender 4.x/5.x. ivtank00.xsi has a documented FBX bone-parent representation difference; strict validation reports PARTIAL, never SUCCESS. JPEG full entropy decode, crash-consistent publication journaling, independent full glTF scene evaluation and deeper FBX material/animation fidelity remain open.
