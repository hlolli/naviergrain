# Building naviergrain

The native build needs CMake 3.16 or later, a C11 compiler and generated
Csound 7 headers. The desktop app also needs a C++17 compiler, Python 3 and Bun.
Ninja is optional. CMake downloads pinned versions of webview and miniaudio
when you enable the desktop target.

## Native

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_CSOUND_BUILD_DIR=/path/to/csound/build \
  -DCSOUND_EXECUTABLE=/path/to/csound/build/csound \
  -DFLUIDGRAIN_DESKTOP=ON -DFLUIDGRAIN_METAL=ON
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

On macOS, open `build/native/naviergrain.app`. The app embeds its UI and
synthesizer. Csound is needed to build and test the plugin, but the app's
audio host uses miniaudio directly.

`FLUIDGRAIN_METAL` requires macOS. On Linux, omit that option and install the
GTK/WebKitGTK development packages required by webview. Windows uses WebView2
and needs `FLUIDGRAIN_NATIVE_WORKER=OFF` for the CPU desktop path, since the
native worker currently uses POSIX threads. Only macOS arm64 has been tested
as a desktop app.

For just the Csound plugin, omit both desktop and Metal options. To select
headers directly, set `HLOLLI_CSOUND_INCLUDE_DIR` to the directory containing
`csdl.h`, `version.h` and `float-version.h`.

Render the example from the repository root:

```sh
/path/to/csound --opcode-lib=build/native/libnaviergrain.dylib \
  -W -s -o build/example.wav examples/naviergrain.csd
```

On Linux, use `libnaviergrain.so`. CMake names the Windows module
`naviergrain.dll`. Installation uses Csound's versioned plugin directory,
which you can override with `HLOLLI_CSOUND_PLUGIN_DIR`.

## Browser

Install Bun, Python 3 and a WASI SDK. This builds the live app:

```sh
python3 tools/build_live_wasm.py --cc /path/to/wasi-sdk/bin/clang \
  --ld /path/to/wasi-sdk/bin/wasm-ld
python3 -m http.server 8765 --bind 127.0.0.1 --directory build/ui
```

Open `http://127.0.0.1:8765`. Audio needs a click on **Evolve** and either
localhost or HTTPS. Cross-origin isolation is unnecessary for this interface.

The separate `web/` directory contains the older Csound/WebGPU workbench.
Its build script is missing from this checkout, so use `ui/` for the browser
app. The workbench source and tests remain for further development.

## Tests

CMake registers the native tests for the backends and Csound libraries it finds.
For address and undefined-behavior checks with Clang or GCC, use a separate
build directory and `-DFLUIDGRAIN_SANITIZE=ON`.

After building the live WASM and native app:

```sh
node tests/test_live_worklet.mjs
node tests/test_spectrogram.mjs
node tests/test_particle_view.mjs
node tests/test_trails_wasm.mjs build/ui/fluidgrain-live.wasm
node tests/test_live_wasm.mjs build/ui/fluidgrain-live.wasm \
  build/native/fluidgrain_live_test
node tests/test_live_deadline.mjs build/ui/fluidgrain-live.wasm 32000 500 1 1
```

The last check measures queue pressure in Node. Test with an audio device too.

The opcode's parameter definitions live in `schema/fluidgrain-v1.json`.
After editing them, run `python3 tools/generate_schema.py`. Use `--check`
to check the generated C, Csound and TypeScript files without changing them.
