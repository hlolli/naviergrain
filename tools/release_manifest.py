#!/usr/bin/env python3
"""Record the files produced by one release build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
parser.add_argument('--csound-commit', required=True)
parser.add_argument('--target', choices=['macos-arm64', 'wasm'], required=True)
parser.add_argument('--compiler', type=Path, required=True)
args = parser.parse_args()
files = {}
for path in sorted(args.directory.iterdir()):
    if path.is_file() and path.name not in ('build.json', 'SHA256SUMS'):
        files[path.name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                            'bytes': path.stat().st_size}
assert files, 'No release files'
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
run = os.environ.get('GITHUB_RUN_ID')
manifest = {
    'source_commit': commit,
    'csound_commit': args.csound_commit,
    'csound_branch': 'develop',
    'target': args.target,
    'metal': args.target == 'macos-arm64',
    'compiler': args.compiler.read_text().strip(),
    'workflow': (f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{run}"
                 if run else None),
    'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'),
    'files': files,
}
if args.target == 'macos-arm64':
    manifest.update(minimum_macos='13.3', architecture='arm64', signing='ad-hoc, not notarized')
name = args.target + '-build.json'
path = args.directory / name
path.write_text(json.dumps(manifest, indent=2) + '\n')
files[name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
(args.directory / (args.target + '-SHA256SUMS')).write_text(''.join(
    f"{info['sha256']}  {name}\n" for name, info in files.items()))
