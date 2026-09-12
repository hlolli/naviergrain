#!/usr/bin/env python3
"""Exercise profiling hooks against the real host and unchanged production audio.

Run with a Python build providing ctypes, after building both plugin targets.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

try:
    import ctypes  # noqa: F401: minimal Python builds can omit the host API bridge.
except ImportError:
    print('profiling check requires a Python build with ctypes; run with full Python')
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('profile_native', ROOT / 'tools/profile_native.py')
profiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--profile-module', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    args = parser.parse_args()
    assert profiler.summary([]) is None
    assert profiler.summary([4, 1, 3, 2]) == dict(
        calls=4, mean_ms=2.5, p50_ms=2, p99_ms=4, p999_ms=4, maximum_ms=4)
    with tempfile.TemporaryDirectory(prefix='fluidgrain-profile-') as directory:
        reports = []
        for instrumented, module in ((False, args.module), (True, args.profile_module)):
            output = Path(directory) / f'{instrumented}.json'
            command = [sys.executable, str(ROOT / 'tools/profile_native.py'),
                       '--module', str(module.resolve()), '--library', str(args.library.resolve()),
                       '--seconds', '1.1', '--prepared', '--block', '37', '--output', str(output)]
            if instrumented:
                command.append('--instrumented')
            result = subprocess.run(command, text=True, capture_output=True, timeout=90)
            assert result.returncode == 0, result.stdout + result.stderr
            report = json.loads(output.read_text())
            assert report['status'] == 'complete'
            assert not output.with_suffix('.json.tmp').exists()
            assert len(report['records']) == 3
            reports.append(report)
        for normal, measured in zip(reports[0]['records'], reports[1]['records'], strict=True):
            assert normal['audio_sha256'] == measured['audio_sha256']
            assert normal['peak_live'] == measured['peak_live']
            assert measured['owned_arena_bytes'] > 0
            assert measured['allocations'] == 0
            assert measured['solver_steps'] == measured['callbacks_containing_fluid_tick']['calls']
            assert measured['solver_steps'] >= 60
            assert measured['maximum_single_solver_step_ms'] > 0
            assert measured['solver_work_per_callback']['maximum_ms'] < measured['all_callbacks']['maximum_ms']
            assert measured['all_callbacks']['calls'] == 1428
            assert normal['allocations'] is None
            assert normal['solver_steps'] is None
            assert measured['underruns'] is None
        bad_output = Path(directory) / 'failure.json'
        # Existing ELF, but not a Csound opcode library: fail after report creation.
        result = subprocess.run(
            [sys.executable, str(ROOT / 'tools/profile_native.py'), '--prepared',
             '--module', str(args.library.resolve()), '--library', str(args.library.resolve()),
             '--seconds', '.1', '--output', str(bad_output)],
            text=True, capture_output=True, timeout=20)
        assert result.returncode != 0
        failure = json.loads(bad_output.read_text())
        assert failure['status'] == 'failed'
        assert failure['records'] == []
        assert failure['failure']['profile'] == 'low'
        assert failure['failure']['completed_calls'] == 0
        # Verify completed records survive a checkpoint with in-progress work.
        report = reports[1]
        report['status'] = 'running'
        report['progress'] = {'profile': 'dense', 'rendered_seconds': 15}
        profiler.write_report(bad_output, report)
        assert json.loads(bad_output.read_text())['records'] == reports[1]['records']
        interrupted_output = Path(directory) / 'interrupted.json'
        command = [sys.executable, str(ROOT / 'tools/profile_native.py'), '--prepared',
                   '--module', str(args.profile_module.resolve()), '--instrumented',
                   '--library', str(args.library.resolve()), '--seconds', '600',
                   '--profiles', 'low', '--progress-seconds', '1',
                   '--output', str(interrupted_output)]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            try:
                process.communicate(timeout=2)
                raise AssertionError('Expected the sustained probe to still be running')
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=10)
                    raise AssertionError('Termination did not finish within ten seconds') from None
        interrupted = json.loads(interrupted_output.read_text())
        assert process.returncode != 0 and interrupted['status'] == 'failed'
        assert interrupted['failure']['completed_calls'] > 0
        assert interrupted['progress']['rendered_seconds'] > 0
        assert 'signal 15' in interrupted['failure']['error']
    print('profiling: exact production audio for all 3 profiles at ksmps=37; '
          'solver/allocation/arena counters and failed-run receipts pass')


if __name__ == '__main__':
    main()
