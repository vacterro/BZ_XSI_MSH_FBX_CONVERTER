# bzconv — Battlezone XSI/MSH converter

Windows C++20 converter for Battlezone 2 XSI and DOCB MSH assets. GLB, glTF
and OBJ export run natively; FBX export uses an installed Blender 4.x/5.x via
the bundled Blender bridge.

Portable release: see `release/README.txt`. CLI: `bzconv --help`.

The GUI includes a **Help** button with in-app usage instructions and an
**Auto-install dependencies** button: it finds an existing Blender 4.x/5.x, or
downloads and unpacks a portable Blender into `%LOCALAPPDATA%\bzconv\deps`
(no administrator rights needed), then sets the Blender path automatically.
GLB/glTF/OBJ export does not need Blender at all.

Current release: **v0.0.2**

Verification statuses are deliberately distinct: `FILE_WRITTEN`,
`STRUCTURALLY_VALID`, `GEOMETRY_VERIFIED`, `FULL_SCENE_VERIFIED`, `PARTIAL_LOSS`.
