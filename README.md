<div align="center">
  <h1>bzconv</h1>
  <p><strong>Battlezone XSI / MSH Asset Converter</strong></p>
  <p>Bring Battlezone models into modern 3D workflows.</p>

  <a href="https://github.com/vacterro/BZ_XSI_MSH_FBX_CONVERTER/releases"><img src="https://img.shields.io/github/v/release/vacterro/BZ_XSI_MSH_FBX_CONVERTER?style=flat-square&color=b69a58" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/Windows-x64-3e6e93?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/C%2B%2B-20-555555?style=flat-square" alt="C++20">
  <img src="https://img.shields.io/badge/Wintage-Golden%20Default-9b8049?style=flat-square" alt="Golden Default">

  <p><a href="https://github.com/vacterro/BZ_XSI_MSH_FBX_CONVERTER/releases/latest"><strong>Download latest release</strong></a> · <a href="#quick-start">Quick start</a> · <a href="CHANGELOG.md">Changelog</a></p>
</div>

## Overview

A Windows C++20 converter for Battlezone 2 / Combat Commander model assets. Use the portable GUI for everyday conversion or the CLI for scripted asset pipelines.

## Supported formats

| Input | Output | Conversion |
|---|---|---|
| XSI / DOCB MSH | GLB / glTF | Native |
| XSI / DOCB MSH | OBJ | Native |
| XSI / DOCB MSH | FBX | Blender 4.x / 5.x bridge |

Blender is **not required** for native GLB, glTF or OBJ export. FBX requires Blender; the GUI can detect an existing installation or download a portable copy into the user's local application data.

## Features

- Drag-and-drop conversion queue, cancellation and saved settings.
- Native model conversion and an integrated FBX bridge.
- Golden Default Wintage interface with built-in help.
- Automatic Blender detection and optional dependency installation without administrator rights.
- Non-overwriting output by default; explicit opt-in to replace files.
- Distinct verification statuses so writing a file is not mistaken for a fully verified conversion.

## Quick start

1. Download the portable package from [Releases](https://github.com/vacterro/BZ_XSI_MSH_FBX_CONVERTER/releases/latest).
2. Extract the complete folder and run `bzconv_gui.exe`.
3. Drop in XSI or MSH files and select the output format.
4. For FBX, select an installed Blender or use **Auto-install dependencies**.
5. Convert and review the verification result.

For automation:

```powershell
.\bzconv.exe --help
```

Keep the bundled bridge, scripts and runtime DLLs beside the executables. See the [portable release guide](release/README.txt).

## Development status

Early-stage development. The public release is v0.0.2; see the changelog for actual changes. Verification distinguishes `FILE_WRITTEN`, `STRUCTURALLY_VALID`, `GEOMETRY_VERIFIED`, `FULL_SCENE_VERIFIED` and `PARTIAL_LOSS`. A converted file is not automatically a lossless or fully verified result.

## Battlezone toolkit

[Map editor — BZ_MAP_MAG](https://github.com/vacterro/BZ_MAP_MAG) · [ODF laboratory — BZ_ODF_SHAMAN](https://github.com/vacterro/BZ_ODF_SHAMAN) · [Project index](https://github.com/vacterro)

<div align="center"><sub>Battlezone assets. Modern tools. Honest verification.</sub></div>
