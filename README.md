# FirstSkinChangeLegit

FirstSkinChangeLegit is an experimental **external viewmodel project for CS2**. It captures the image displayed on screen, identifies the area occupied by the original weapon, reconstructs the background in that area, and draws another model in an overlay. The program does not read or modify game memory.

The visual result is already satisfying: the weapon follows the scene, picks up lighting and reflections from the map, and often looks like part of the game. The main area that still needs improvement is **mask synchronization** with the original viewmodel’s animations and movement. When the mask runs ahead or falls behind, parts of the original weapon may become visible.

## How it works

1. Captures screen frames and tracks the configured inputs.
2. Creates a viewmodel mask from green-screen recordings and, when available, an ONNX segmentation model.
3. Uses previous frames and NVIDIA Optical Flow to try to reconstruct the background hidden by the weapon. Where reliable information is unavailable, it applies an alternative fill method.
4. Renders the selected model in a Direct3D 11 overlay, with animations, lighting, and reflections derived from the game image.

The menu opens with **HOME** by default. Shortcuts, resolution, aspect ratio, black bars, weapon position, and other options can be adjusted there.

## Project status

This is a prototype under development, particularly with respect to synchronization between capture, masking, and animation. Its appearance may vary depending on resolution, FPS, lighting, the recording used for the mask, and keybind settings. There is no official integration with CS2.

## Build and run

Requirements: Windows, Visual Studio 2022 Build Tools with C++, CMake, and a Direct3D 11-compatible GPU. The third-party components used by the project are in `native/third_party`, along with their respective license notices.

Run `native\build.bat`. Then launch `native\build\Release\vmoverlay.exe`. The browser-based preview interface can be opened with `abrir.bat`.

The public ZIP contains **only code and third-party dependencies**. It does not include game models, textures, animations, captures, masks, recordings, or AI weights. To display a viewmodel, provide files you have the right to use in the directories expected by the code (`models/view`, `models/gloves`, `models/cs2vm`, and `textures`). The `native/skins.tsv` and `skins.json` catalogs are empty in the public package.

### Sources used during development

The development history records these sources for assets used in the local version:

- [CS2 Spraylab](https://spraylab.pages.dev/): viewmodels in `models/view`, gloves in `models/gloves`, textures in `textures/cosmetics`, and data used in the skin catalog.
- [AstraStrike](https://astrastrike.fun/): viewmodel animations used in `models/cs2vm`.

These files **are not included in this repository**. Listing their sources does not grant permission to download, use, or redistribute the assets; check the applicable permissions before using them. FirstSkinChangeLegit is not officially affiliated with Valve, CS2, or these websites.

## Screenshots

Images showing the project in action without redistributing model or texture files.<br>
https://github.com/user-attachments/assets/11235c22-39e0-4a5f-b5a5-4876e8672f0b

## Licenses

License notices for third-party libraries are included with their files. A license for the project’s original code still needs to be chosen before publication.
