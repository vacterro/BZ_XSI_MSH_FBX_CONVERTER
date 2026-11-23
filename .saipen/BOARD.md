# Board

<!-- Ticket shape is RFC § 1.2's, exactly: a checkbox, the T-### id, a
     description, then only the fields that apply, space-pipe separated.
     Shown here WITHOUT its leading "- " on purpose (see below):

       [ ] T-001 short description | verify: pytest -q

     Other legal fields (RFC § 1.2): the dependency one, taking a
     comma-separated list of T-### this ticket waits on; owner and
     claim_time for claims (§ 1.4); blocker for facts + dead ends; verify as
     shown above. Named rather than shown here on purpose -- see below.

     A real line starts with "- ". Checkbox: [ ] open, [/] in progress
     (## DOING), [x] done (## DONE). A status change MOVES the line between
     sections -- cut and paste, never copy, or the same id ends up under two
     headings. All four headings below are required, even while empty.

     Why the example is de-fanged: neither validator skips HTML comments, so
     anything ticket-shaped in here is read as a real ticket on a brand-new,
     untouched board. Two separate traps, both hit for real while writing
     this very file: a full checkbox line parses as a live ticket, and the
     dependency field followed by an id is flagged as a dangling reference
     even without a leading dash -- tests/validate.sh scans for that field
     across the whole file, not only ticket lines, making it stricter here
     than tools/validate.py. So: no leading dash on any example, and never
     write that field name next to a concrete id anywhere in this file. -->

## DOING

## TODO

## DONE
- [x] T-17 [P2] Сделай exe GUI версию и запуш нормальную 0.0.1 | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-013 | owner: saipen | claim_time: 2026-11-23T20:33:16Z | closure_mode: own_patch | publication_state: pending
- [x] T-16 [P2] Account for post-T-14 edits to src/cli/main.cpp, tools/source_fidelity_check.py and tools/regression_check_T14.py (all after E-272) with canonical Work/LOG provenance; they are unsourced changes on a DONE tree | verify: every post-T-14 source edit is attributed to a canonical ticket with LOG evidence or reverted with proof | owner: saipen | claim_time: 2026-11-23T19:34:29Z | closure_mode: own_patch | publication_state: pending
- [x] T-15 [P1] Reconcile build/bzconv.exe (92ee3b12) with release/bzconv.exe + RELEASE_EVIDENCE.json artifact_sha256 (27cf4b65); rebuild/refresh release from last reviewed tree or restore build, then re-verify parity | verify: sha256sum build/bzconv.exe release/bzconv.exe match RELEASE_EVIDENCE.json artifact_sha256 for both paths | owner: saipen | claim_time: 2026-11-23T19:24:34Z | closure_mode: own_patch | publication_state: pending
- [x] T-14 [P2] Night Shift 2: reproduce and repair P0 OutputBundle recovery/data-safety and source-fidelity false-success defects, verify with RED/GREEN and real assets, th... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-012 | owner: saipen | claim_time: 2026-11-23T19:03:28Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-13 [P2] Night Shift 2: reproduce and repair P0 OutputBundle recovery data-safety and source-fidelity false-success defects, verify on real assets, ship proven release | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-011 | owner: saipen | claim_time: 2026-11-23T17:59:14Z | closure_mode: own_patch | publication_state: pending
- [x] T-12 [P3] Add a repository-style integration test for bzconv recover <dir> CLI dispatch (OutputBundle core recovery is already covered by two C++ tests and a manual smoke; only the CLI argument/dispatch path lacks an automated case) | verify: build/bzconv_tests.exe includes a case that drives the recover CLI entry point and passes | owner: saipen | claim_time: 2026-11-23T16:57:16Z | closure_mode: own_patch | publication_state: pending
- [x] T-11 [P2] NIGHT SHIFT 2: repair glTF evaluator P0 oracle bugs (world matrix order, accessor byteOffset/byteStride, vertex/primitive-loss detection, animation value com... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-010 | owner: saipen | claim_time: 2026-11-23T16:39:16Z | closure_mode: own_patch | publication_state: pending
- [x] T-10 [P0] BUILD user directive: regression pair: run contract .saipen/evidence/regression_contract_T9.json (oracle tools/regression_check_T9.py; before-snapshot .saipen/evidence/pre/glb_exporter.t9_pre.cpp substitutes src/bzconverter/exporters/glb_exporter.cpp) | verify: agent FIT/impact pass recorded; normal SCOUT, BUILD, VERIFY, REVIEW and SHIP/DONE gates pass | owner: saipen | claim_time: 2026-11-23T07:08:19Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-9 [P2] Night shift 2 — world-space fidelity, GLTF validity and release hardening | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-009 | owner: saipen | claim_time: 2026-11-23T07:05:51Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-8 [P2] Fix SAIHANDOFF-10 confirmed P0: model-space normal transform direction, parent-order-independent world matrix resolver, skinned model-space bind-pose localiz... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-008 | owner: saipen | claim_time: 2026-11-23T01:09:53Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-7 [P2] Fix new confirmed P0 defects from SAIHANDOFF-9: GLB double transform for vertices_in_model_space, core input/output identity guard, silent texture loss diagn... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-007 | owner: saipen | claim_time: 2026-11-23T00:29:42Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-6 [P2] Fix confirmed P0 defects from SAIHANDOFF: GLB influence cap + weight renormalization, GLB loss diagnostics to status, transactional multi-file output bundles... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-006 | owner: saipen | claim_time: 2026-11-23T00:19:50Z | regression: required | regression_evidence: executed | closure_mode: own_patch | publication_state: pending
- [x] T-5 [P2] SAIHANDOFF-5 Night Shift 2: fix GLB silent skin weight loss and artificial bone assignment, companion node-index remapping, GUI output-path drift, queue buff... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-005 | owner: saipen | claim_time: 2026-11-22T22:22:39Z | closure_mode: own_patch | publication_state: pending
- [x] T-4 [P2] status | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-004 | owner: saipen | claim_time: 2026-11-22T21:33:55Z | closure_mode: own_patch | publication_state: pending
- [x] T-3 [P2] Night Shift 2 per SAIHANDOFF-4: repair FBX skin-weight loss, false-pass validation, GUI worker lifetime, safe output replacement; complete animation-only com... | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-003 | owner: saipen | claim_time: 2026-11-22T21:26:17Z | closure_mode: own_patch | publication_state: pending
- [x] T-2 [P2] Execute SAIHANDOFF-3: Fix CLI UTF-8, atomic write, skinning, MSH verification, and build Golden Default GUI | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-002 | owner: saipen | claim_time: 2026-11-22T20:27:31Z | closure_mode: own_patch | publication_state: pending
- [x] T-1 [P2] saipen init - продолжи тогда. | verify: the requested change is present and demonstrated against the user own description of it | request_witness: model_supplied | source_receipts: SRC-001 | owner: saipen | claim_time: 2026-11-22T20:02:03Z | closure_mode: own_patch | publication_state: pending

## BLOCKED
