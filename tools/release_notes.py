#!/usr/bin/env python3
"""Write release notes from the build's recorded source versions."""
import os

repo = os.environ['GITHUB_REPOSITORY']
commit = os.environ['GITHUB_SHA']
csound = os.environ['CSOUND_COMMIT']
run = os.environ['GITHUB_RUN_ID']
print(f'''naviergrain for Apple Silicon, with Metal audio rendering and an About / Quit menu.

- **macOS app:** unzip `naviergrain-macos-arm64.zip` and open naviergrain. Requires macOS 13.3 or later on arm64 (M1 and later, including M5). Self-contained; no Csound install needed. Ad-hoc signed, without Apple notarization.
- **Csound WASM plugin:** `naviergrain-csound-wasm.zip` contains the opcode plugin, score includes and an example. Built and tested with Csound `develop` at [{csound[:12]}](https://github.com/csound/csound/commit/{csound}). Use a matching Csound 7 WASM host; this is separate from the live app's WASM engine.

[Source commit](https://github.com/{repo}/commit/{commit}) · [Build and test run](https://github.com/{repo}/actions/runs/{run})

`SHA256SUMS` checks the downloads. The build JSON files record source versions and compiler details. Verify GitHub's signed build record with:

```sh
gh attestation verify naviergrain-macos-arm64.zip --repo {repo}
gh attestation verify naviergrain-csound-wasm.zip --repo {repo}
```

Created by Hlöðver Sigurðsson. [GitHub](https://github.com/hlolli) · [Twitter / X](https://x.com/Hlolli)
''')
