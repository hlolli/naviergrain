#!/usr/bin/env python3
"""Identify a local build without inventing a missing upstream Git revision."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--csound-root', type=Path, default=ROOT.parent / 'csound')
    args = parser.parse_args()
    cache = {}
    for line in (args.build / 'CMakeCache.txt').read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line:
            key, value = line.split('=', 1)
            cache[key.split(':')[0]] = value
    root = args.csound_root.resolve()
    includes = root / 'build/include'
    headers = {p.name: digest(p) for p in sorted(includes.glob('*.h'))}
    source_files = [root / 'CMakeLists.txt']
    for directory in ('include', 'Engine', 'OOps', 'Opcodes', 'Top', 'cmake'):
        source_files.extend(p for p in (root / directory).rglob('*') if p.is_file())
    manifest = ''.join(f'{digest(p)}  {p.relative_to(root)}\n' for p in sorted(source_files))
    compiler = cache['CMAKE_C_COMPILER']
    report = {
        'date': '2026-09-10',
        'upstream_revision': None,
        'qualification': 'Observed artifacts only; no proof this runtime was built from the observed source snapshot.',
        'csound_root': str(root),
        'source_snapshot': {'files': len(source_files), 'sha256_manifest': hashlib.sha256(manifest.encode()).hexdigest(),
                            'scope': ['CMakeLists.txt', 'include', 'Engine', 'OOps', 'Opcodes', 'Top', 'cmake']},
        'generated_headers_sha256': headers,
        'artifacts_sha256': {str(p): digest(p) for p in (root / 'build/csound', root / 'build/libcsound64.so.7.0', args.build / 'libnaviergrain.so')},
        'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
        'build': {key: cache.get(key) for key in ('CMAKE_C_COMPILER', 'CMAKE_BUILD_TYPE', 'CMAKE_C_FLAGS',
                                                'CMAKE_C_FLAGS_RELWITHDEBINFO', 'CMAKE_MAKE_PROGRAM')},
        'runtime': {'version': '7.0.0', 'MYFLT': 'double (8 bytes)', 'multicore_build': True,
                    'tests': 'offline stereo, no realtime backend, explicit --opcode-lib'},
        'wasm_verified': False,
    }
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
