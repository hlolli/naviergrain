"""Build the live C engine for browsers with a WASI SDK compiler."""
import argparse
import os
from pathlib import Path
import subprocess
from build_ui import ROOT, build

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default=os.environ.get("WASI_CC", "clang"))
parser.add_argument("--sysroot")
parser.add_argument("--ld", help="Optional absolute path to wasm-ld")
parser.add_argument("--output", type=Path, default=ROOT / "build/ui")
parser.add_argument("--bun", default="bun")
args = parser.parse_args()
build(args.output, bun=args.bun)
sources = ["core", "field", "particles", "resampler", "live"]
exports = ["create", "destroy", "control", "render", "audio", "view", "stats", "gpu", "cpu", "particles", "spectrum"]
command = [args.cc, "--target=wasm32-unknown-wasi", "-O3", "-std=c11", "-nostartfiles",
           "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
           "-Wl,--no-entry", "-Wl,--export-memory", "-Wl,-z,stack-size=1048576",
           "-Wl,--initial-memory=67108864", "-Wl,--max-memory=268435456"]
if args.sysroot:
    command += ["--sysroot=" + args.sysroot]
if args.ld:
    command += ["-fuse-ld=" + args.ld]
command += ["-Wl,--export=fg_live_" + name for name in exports]
command += [str(ROOT / "src" / ("fluidgrain_" + name + ".c")) for name in sources]
command += ["-lm", "-o", str(args.output / "fluidgrain-live.wasm")]
subprocess.run(command, check=True)
