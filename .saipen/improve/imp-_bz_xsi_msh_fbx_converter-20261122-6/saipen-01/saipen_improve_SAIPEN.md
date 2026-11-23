agent: saipen-01
role: core
model_or_runtime: unknown
project: _bz_xsi_msh_fbx_converter
saipen_version: 8.1.1
protocol_fingerprint: sha256:b3257b7f73248baff32828e41e3b5ecf72978c4a1043f4e586be969920ff81e6
source_head: no-git
source_tree_fingerprint: no-git-tree-v1:2bf5ad4eb61880888c057faf73638b7c22875299d20d59de2672b4a0551b8caa
discovery_model: no-git-tree-v1
context_scope: SAIPEN audit, phase DONE
context_available: partial
report_status: complete

## RUN 1

run_source_head: no-git
run_source_tree_fingerprint: no-git-tree-v1:2bf5ad4eb61880888c057faf73638b7c22875299d20d59de2672b4a0551b8caa

Live-tree audit after T-9/T-10: suite 32/32 PASS plus 6/6 T-9 regression cases (45 PASS lines, 0 FAIL); measured regression pair FAIL->PASS (record regression-execution-d8ec01ab, before 7 ran/2 failed/exit1, after 7 ran/0 failed/exit0, verifier 53a0c642); independent glTF evaluator PASS on fvtank_MORPH.xsi and ivtank00.msh GLBs with world bbox exact match to source inspect; adversarial matrix 12/12 on fvtank.glb and all-present on ivtank00.glb; release/ directory clean (runtime, bridge, docs only) and release/bzconv.exe == build/bzconv.exe (sha256 6cc99290); no stray temp outputs in repo.
IMP-001 [P3] [OTHER] [observed] [note]
expected: exporter unreferenced shared mesh 0 in the two-instance fixture carries no orphan glTF mesh asset
actual: instanced model-space export leaves the shared (node-unreferenced) glTF mesh in the meshes array; harmless to renderers but wasteful bytes and a validator nit
evidence: t9 instance fixture dump - nodes A/B reference shared_A/shared_B, mesh 'shared' has no node reference
IMP-002 [P3] [OTHER] [observed] [note]
expected: regression oracle documents the unittest summary grammar the engine parser requires
actual: tools/regression_check_T9.py now tolerates suite failure at test level (needed for the pair gate) but the requirement was discovered only through REGRESSION_EXECUTION_INVALID refusals
evidence: .saipen/evidence/regression/regression-execution-d8ec01ab635e46d9ae9daf83937adfdb.json runs[].stderr
