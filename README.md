# bzconv — Battlezone XSI/MSH converter

Windows C++20 converter for Battlezone 2 XSI and DOCB MSH assets. GLB, glTF
and OBJ export run natively; FBX export uses an installed Blender 4.x/5.x via
the bundled Blender bridge.

Portable release: see `release/README.txt`. CLI: `bzconv --help`.

Current release: **v1.2.0**

Verification statuses are deliberately distinct: `FILE_WRITTEN`,
`STRUCTURALLY_VALID`, `GEOMETRY_VERIFIED`, `FULL_SCENE_VERIFIED`, `PARTIAL_LOSS`.
