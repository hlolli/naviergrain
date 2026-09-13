#!/usr/bin/env python3
"""Check bundle contents, repeatability and failed-publication behavior."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_native', ROOT / 'tools/package_native.py')
bundler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundler)


def main():
    with tempfile.TemporaryDirectory(prefix='naviergrain-package-') as directory:
        root = Path(directory).resolve()
        for name in (*bundler.REQUIRED_SOURCE, *bundler.AUDIO_FILES,
                     *bundler.LOCAL_VIDEO_TOOLS, '.local/development-archive/README.md',
                     'docs/diagnostic-audio.json', 'src/naviergrain_grains.metal',
                     'desktop/main.cpp', 'cmake/desktop.cmake', 'ui/main.js',
                     'ui/fonts/ibmplexsans-OFL.txt',
                     'tests/test_host.c', 'examples/cpu-worker.c', 'tools/package_native.py', 'build/libnaviergrain.so',
                     'Custom.cmake', '.env', 'build/libnaviergrain_profile.so',
                     'build/libnaviergrain_shuffle.so', 'tools/__pycache__/junk.pyc'):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        module = root / 'build/libnaviergrain.so'
        first, second = root / 'first.tar.gz', root / 'second.tar.gz'
        bundler.package(root, module, first)
        bundler.package(root, module, second)
        assert first.read_bytes() == second.read_bytes()
        with tarfile.open(first) as archive:
            members = archive.getmembers()
            assert all(m.isfile() and m.name.startswith(bundler.PREFIX + '/') for m in members)
            manifest = json.load(archive.extractfile(f'{bundler.PREFIX}/MANIFEST.json'))
            names = set(manifest['files'])
            assert not any('Custom.cmake' in name or '.env' in name or
                           'libnaviergrain_profile' in name or 'libnaviergrain_shuffle' in name or
                           '__pycache__' in name for name in names)
            assert set(bundler.REQUIRED_SOURCE) | set(bundler.AUDIO_FILES) <= names
            assert 'examples/cpu-worker.c' in names
            assert names.isdisjoint(bundler.LOCAL_VIDEO_TOOLS)
            assert 'docs/diagnostic-audio.json' not in names
            assert not any(name.startswith('.local/') for name in names)
            assert {'src/naviergrain_grains.metal', 'desktop/main.cpp',
                    'cmake/desktop.cmake', 'ui/main.js',
                    'ui/fonts/ibmplexsans-OFL.txt'} <= names
            assert len(members) == len(names) + 1
            for name, metadata in manifest['files'].items():
                data = archive.extractfile(f'{bundler.PREFIX}/{name}').read()
                assert metadata == dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        before = first.read_bytes()
        missing = root / bundler.AUDIO_FILES[0]
        missing.unlink()
        try:
            bundler.package(root, module, first)
            raise AssertionError('Missing audio must fail')
        except ValueError:
            pass
        assert first.read_bytes() == before
        missing.symlink_to(module)
        try:
            bundler.package(root, module, first)
            raise AssertionError('Symlinked input must fail')
        except ValueError:
            pass
        assert first.read_bytes() == before
        missing.unlink()
        missing.write_text('restored')
        try:
            bundler.package(root, root / 'build/libnaviergrain_profile.so', first)
            raise AssertionError('Diagnostic binary must not be packaged as production')
        except ValueError:
            pass
        assert first.read_bytes() == before
        assert not list(root.glob('*.tmp'))
    print('Bundle checks pass: exact manifest, repeatable archive, scoped inputs, failure preserves output')


if __name__ == '__main__':
    main()
