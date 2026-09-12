#!/usr/bin/env python3
"""Bundle native sources, a module and diagnostic audio with file hashes."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'naviergrain-native-dev'
SOURCE_PATTERNS = (
    'src/*.[ch]', 'src/*.inc', 'src/*.mm', 'src/*.metal', 'src/*.cu',
    'include/*.h', 'include/*.inc', 'schema/*.json', 'cmake/*',
    'desktop/*.cpp', 'desktop/*.hpp', 'desktop/*.c', 'ui/*.js', 'ui/*.css',
    'ui/*.html', 'ui/fonts/*', 'web/*.ts', 'web/*.wgsl', 'web/*.html',
    'examples/*.c', 'examples/*.csd', 'examples/*.inc', 'tests/*.c',
    'tests/*.py', 'tests/*.inc', 'tests/*.mjs', 'tests/*.ts',
    'tools/*.py', 'tools/*.ts', 'tools/*.c', 'tools/*.mjs', 'tools/*.js',
    'tools/*.wgsl', 'docs/*.md',
)
LOCAL_VIDEO_TOOLS = {
    'tools/score_study.py', 'tools/render_study.c',
    'tools/render_study_video.mm', 'tools/finish_study.py',
}
REQUIRED_SOURCE = (
    'CMakeLists.txt', 'README.md', 'LICENSE', 'COPYRIGHT',
    '.gitignore', '.clang-format', 'tokens.css',
    'src/fluidgrain_opcode.c',
    'src/fluidgrain_core.c', 'include/fluidgrain_schema.h',
    'include/fluidgrain.inc', 'schema/fluidgrain-v1.json',
    'include/naviergrain.inc', 'docs/build.md', 'tools/scheduler_plan.c',
)
SCENES = ('vortex-bloom', 'honey-drift', 'ion-blizzard', 'gravity-silk')
AUDIO_FILES = (
    'build/fluidgrain-diagnostic.wav', 'build/diagnostic.log',
    *(f'build/spatial-comparisons/{name}-spatial-ab.{extension}'
      for name in SCENES for extension in ('wav', 'mp3')),
)


def collect_files(root, module):
    paths = set(REQUIRED_SOURCE) | set(AUDIO_FILES)
    for pattern in SOURCE_PATTERNS:
        paths.update(path.relative_to(root).as_posix() for path in root.glob(pattern))
    paths.difference_update(LOCAL_VIDEO_TOOLS)
    files = {name: root / name for name in paths}
    if module.name not in ('libnaviergrain.so', 'libnaviergrain.dylib', 'naviergrain.dll',
                           'libfluidgrain.so', 'libfluidgrain.dylib', 'fluidgrain.dll'):
        raise ValueError('Select the production naviergrain module, not a diagnostic module')
    files[f'build/{module.name}'] = module
    for name, path in files.items():
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'Missing regular bundle input: {name}')
        # Never follow a symlinked directory into unrelated workspace data.
        if any(parent.is_symlink() for parent in path.absolute().parents):
            raise ValueError(f'Symlinked bundle input directory: {name}')
    return files


def package(root, module, output):
    files = collect_files(root, module)
    if output.resolve() in (path.resolve() for path in files.values()):
        raise ValueError('Output must not replace a bundle input')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Assemble in the destination filesystem; do not replace a previous bundle
    # until every required input has been read and the archive has closed.
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix='.tmp', delete=False) as temporary:
        staging = Path(temporary.name)
    try:
        with staging.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode='w') as archive:
                manifest = {
                    'format_version': 1,
                    'qualification': 'Development build',
                    'compatibility': 'Binary is host-specific; rebuild against matching Csound 7 generated headers',
                    'files': {},
                }

                def add(name, data):
                    info = tarfile.TarInfo(f'{PREFIX}/{name}')
                    info.size = len(data)
                    info.mode = 0o644
                    archive.addfile(info, io.BytesIO(data))

                for name, path in sorted(files.items()):
                    data = path.read_bytes()
                    add(name, data)
                    manifest['files'][name] = {
                        'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                    }
                add('MANIFEST.json', (json.dumps(manifest, indent=2) + '\n').encode())
        staging.replace(output)
    finally:
        staging.unlink(missing_ok=True)
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, default=ROOT / 'build/libnaviergrain.so')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/naviergrain-native-dev.tar.gz')
    args = parser.parse_args()
    count = package(ROOT, args.module, args.output)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(f'{args.output}: {count} files, sha256 {digest}')


if __name__ == '__main__':
    main()
