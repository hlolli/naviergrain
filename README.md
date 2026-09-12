# naviergrain

Fluid motion made audible. naviergrain follows particles through a Navier–Stokes
simulation and turns their movement into sound, with each visible grain playing
a short cosine wave. Height sets frequency. Horizontal position sets stereo pan,
and faster motion shortens the grains.

[![Watch naviergrain: Continuum Study 01 on YouTube](https://i.ytimg.com/vi/kj7AvNTSg-4/maxresdefault.jpg)](https://www.youtube.com/watch?v=kj7AvNTSg-4)

Change viscosity, forcing or attraction and listen to the field change. The
spiral, waveform and spectrogram share one canvas, with controls in the corner.
The simulation uses a periodic 2D grid mapped into 3D space.

The desktop app and browser share a WebGL interface and C synthesis engine.
macOS can render grains with Metal. The browser runs the engine as WebAssembly
and plays audio through an AudioWorklet. A Csound 7 opcode also supports
granulation of sample tables.

## Build

For the macOS app, install CMake, Ninja, Python 3 and Bun. You also need
generated headers from a Csound 7 build:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_CSOUND_BUILD_DIR=/path/to/csound/build \
  -DFLUIDGRAIN_DESKTOP=ON -DFLUIDGRAIN_METAL=ON
cmake --build build/native
open build/native/naviergrain.app
```

The app includes its interface and audio engine, so it runs without a web
server or a separate Csound installation. The first build downloads webview
and miniaudio. Build options retain the older `FLUIDGRAIN_` prefix.

[Build and test instructions](docs/build.md) cover the browser and Csound plugin.

## Using it

Press **Evolve** to start. Drag the canvas to orbit, scroll to zoom, and drag
a control to change its value. Double-click a control to reset it.

The live app has 256 grain voices. When those are busy, it skips new starts
and shows the count. Dense settings still depend on the machine's audio budget.
macOS arm64 has been tested. Linux and Windows desktop builds need testing.

For Csound, start with [examples/naviergrain.csd](examples/naviergrain.csd).
[Opcode notes](docs/opcodes.md) explain its sample-based controls, which differ
from the live app's fixed position-to-frequency mapping.

See [sound and graphics](docs/live-interface.md) for the mapping, or
[engine notes](docs/architecture.md) for the solver and worker layout.

## License

Copyright (C) 2026 Hlöðver Sigurðsson. Licensed under
[GPL-3.0-or-later](LICENSE). Third-party components retain their own licenses.
Music and video rendered with naviergrain can be licensed separately.
