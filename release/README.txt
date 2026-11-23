bzconv portable Windows release
===============================
Requires: Windows 10/11 x64. FBX export requires Blender 4.x/5.x installed
(use GUI Blender path or CLI --blender). GLB/glTF/OBJ are native, no Blender.

Files:
  bzconv_gui.exe  - Golden Default Wintage GUI (queue, drag-drop, cancel, settings)
  bzconv.exe      - CLI (see: bzconv --help; --overwrite opts in replacing outputs)
  bz_fbx_bridge.py + scripts/ - Blender FBX bridge (keep beside EXE)
  MinGW runtime DLLs - required beside EXE

Safe defaults: existing outputs are NOT replaced unless --overwrite (CLI) or
"Overwrite existing" (GUI) is checked. Verification failures are never green.
