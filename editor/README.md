# quakerayeditor

Standalone QuakeRay editor executable. It is a host application around
`qray::renderer`, not part of `quakeray.exe`.

The current target is the application shell and CMake integration. Scene
loading, the shared editor core, material/light authoring, and the viewport
render loop are extracted next from the Quake host; the coupled
`Quake/qr_editor.c` and `Quake/qr_gui.cpp` files are not copied into the SDK.

Build it with:

```powershell
cmake -S . -B build -DQR_BUILD_EDITOR=ON -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi
cmake --build build --config Debug --target quakerayeditor
```

The executable is `quakerayeditor.exe`.
