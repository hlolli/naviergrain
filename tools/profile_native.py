#!/usr/bin/env python3
"""Measure real csoundPerformKsmps calls offline, including fluid-tick spikes.

This is a development profiler, not a hardware audio/underrun qualification.
Requires the matching Csound 7 shared library, no Python packages.
"""
import argparse
from array import array
import ctypes as C
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def summary(values):
    ordered = sorted(values)
    if not ordered:
        return None
    return dict(calls=len(ordered), mean_ms=statistics.mean(ordered),
                p50_ms=ordered[math.ceil(.50*len(ordered))-1],
                p99_ms=ordered[math.ceil(.99*len(ordered))-1],
                p999_ms=ordered[math.ceil(.999*len(ordered))-1],
                maximum_ms=ordered[-1])


def write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)


def resident_bytes():
    status = Path('/proc/self/status')
    if status.exists():
        for line in status.read_text().splitlines():
            if line.startswith('VmRSS:'):
                return int(line.split()[1]) * 1024
    return None


def source_hashes():
    paths = [*ROOT.glob('src/*.[ch]'), *ROOT.glob('include/*'),
             ROOT / 'schema/naviergrain-v1.json',
             ROOT / 'CMakeLists.txt', ROOT / 'tests/profile_hooks.c', Path(__file__)]
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(paths)}


def build_metadata(module):
    cache = module.parent / 'CMakeCache.txt'
    if not cache.exists():
        return None
    keys = {'CMAKE_C_COMPILER', 'CMAKE_BUILD_TYPE', 'CMAKE_C_FLAGS',
            'CMAKE_C_FLAGS_RELWITHDEBINFO', 'CMAKE_C_FLAGS_RELEASE', 'NAVIERGRAIN_SANITIZE'}
    values = {}
    for line in cache.read_text().splitlines():
        key = line.split(':', 1)[0]
        if key in keys and '=' in line:
            values[key] = line.split('=', 1)[1]
    compiler = values.get('CMAKE_C_COMPILER')
    if compiler:
        result = subprocess.run([compiler, '--version'], capture_output=True, text=True, timeout=10)
        values['compiler_version'] = result.stdout.splitlines()[0] if result.returncode == 0 else None
    return values


def main():
    def interrupted(signum, _frame):
        raise InterruptedError(f'Interrupted by signal {signum}')

    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, default=ROOT.parent / 'csound/build/libcsound64.so.7.0')
    parser.add_argument('--module', type=Path, default=ROOT / 'build/libnaviergrain.so')
    parser.add_argument('--seconds', type=float, default=2)
    parser.add_argument('--sample-rate', type=int, choices=(44100, 48000, 96000), default=48000)
    parser.add_argument('--block', type=int, default=64)
    parser.add_argument('--prepared', action='store_true',
                        help='Pre-roll a gated persistent instrument before timing delivered callbacks')
    parser.add_argument('--profiles', nargs='+', choices=('low', 'normal', 'dense'),
                        default=['low', 'normal', 'dense'])
    parser.add_argument('--instrumented', action='store_true',
                        help='Require the non-installed libnaviergrain_profile module for solver/allocation counters')
    parser.add_argument('--progress-seconds', type=float, default=30,
                        help='Wall-clock checkpoint interval; writes partial report and prints progress')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/callback-profile.json')
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not .1 <= args.seconds <= 3600 or not 1 <= args.block <= 4096:
        parser.error('Use .1–3600 seconds and a block size of 1–4096')
    if not math.isfinite(args.progress_seconds) or not 1 <= args.progress_seconds <= 60:
        parser.error('Progress interval must be 1–60 seconds')
    if len(set(args.profiles)) != len(args.profiles):
        parser.error('Profiles must not repeat')
    probe = None
    probe_values = (C.c_double * 6)()
    if args.instrumented:
        probe = C.CDLL(str(args.module.resolve()))
        probe.ng_profile_begin.argtypes, probe.ng_profile_begin.restype = [], None
        probe.ng_profile_end.argtypes, probe.ng_profile_end.restype = [C.POINTER(C.c_double)], None
    lib = C.CDLL(str(args.library.resolve()))
    pointer = C.c_void_p
    signatures = {
        'csoundInitialize': (C.c_int32, [C.c_int32]),
        'csoundCreate': (pointer, [pointer, C.c_char_p]),
        'csoundDestroy': (None, [pointer]),
        'csoundGetSizeOfMYFLT': (C.c_int32, []),
        'csoundCreateMessageBuffer': (None, [pointer, C.c_int32]),
        'csoundDestroyMessageBuffer': (None, [pointer]),
        'csoundCompileCSD': (C.c_int32, [pointer, C.c_char_p, C.c_int32, C.c_int32]),
        'csoundSetOption': (C.c_int32, [pointer, C.c_char_p]),
        'csoundStart': (C.c_int32, [pointer]),
        'csoundPerformKsmps': (C.c_int32, [pointer]),
        'csoundGetControlChannel': (C.c_double, [pointer, C.c_char_p, C.POINTER(C.c_int32)]),
        'csoundSetControlChannel': (None, [pointer, C.c_char_p, C.c_double]),
        'csoundGetSpout': (C.POINTER(C.c_double), [pointer]),
        'csoundGetFirstMessage': (C.c_char_p, [pointer]),
        'csoundPopFirstMessage': (None, [pointer]),
        'csoundGetMessageCnt': (C.c_int32, [pointer]),
    }
    for name, (result, arguments) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, arguments
    if lib.csoundGetSizeOfMYFLT() != 8:
        raise RuntimeError('This profiler requires the pinned double-precision Csound 7 API')
    # This API host owns signals and explicit teardown. Otherwise Csound's
    # process-wide handler replaces Python's interruption/checkpoint handler.
    if lib.csoundInitialize(1 | 2) < 0:  # NO_SIGNAL_HANDLER | NO_ATEXIT
        raise RuntimeError('Csound process initialization failed')

    def channel(host, name):
        error = C.c_int32()
        value = lib.csoundGetControlChannel(host, name.encode(), C.byref(error))
        if error.value:
            raise RuntimeError(f'Missing diagnostic channel {name}: {error.value}')
        if not math.isfinite(value):
            raise RuntimeError(f'Non-finite diagnostic channel {name}')
        return value

    records = []
    cpu = platform.processor()
    cpuinfo = Path('/proc/cpuinfo')
    if cpuinfo.exists():
        cpu = next((line.split(':', 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                    if line.startswith('model name')), cpu)
    report = dict(scope='Offline Csound API callback probe; no hardware audio device or underrun measurement',
                  status='running', started_utc=datetime.now(timezone.utc).isoformat(),
                  requested_seconds_per_profile=args.seconds, requested_profiles=args.profiles,
                  instrumented=args.instrumented, cpu=cpu, platform=platform.platform(),
                  python=platform.python_version(),
                  cpu_affinity=sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
                  module_sha256=hashlib.sha256(args.module.read_bytes()).hexdigest(),
                  library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(),
                  sources_sha256=source_hashes(),
                  build=build_metadata(args.module.resolve()),
                  sampler='Blackman windowed-sinc, 33 bands, 128 fractional phases', records=records)
    write_report(args.output, report)
    # grid, emitters, pool, base rate, grain milliseconds, pressure passes
    profiles = {'low': (16, 128, 128, 80, 60, 64),
                'normal': (32, 512, 512, 600, 100, 64),
                'dense': (32, 1024, 2048, 2000, 500, 128)}
    for name, (grid, emitters, voices, rate, grain_ms, pressure) in profiles.items():
        if name not in args.profiles:
            continue
        profile_start = time.perf_counter()
        last_progress = profile_start
        rss_before = resident_bytes()
        host = lib.csoundCreate(None, None)
        if not host:
            raise RuntimeError('csoundCreate failed')
        lib.csoundCreateMessageBuffer(host, 0)
        times, fluid_times, solver_times = array('d'), array('d'), array('d')
        audio_hash = hashlib.sha256()
        allocations = solver_steps = arena_bytes = 0
        maximum_step_ms = peak_relative_divergence = peak_energy = 0
        peak_live = peak_audio = numeric = drops = 0
        log = []
        try:
            option = f'--opcode-lib={args.module.resolve()}'.encode()
            if lib.csoundSetOption(host, option):
                raise RuntimeError('Could not select the plugin')
            reader = 'aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl'
            if args.prepared:
                reader = ('kRun chnget "run"\n'
                          'aL, aR, kStats[] NaviergrainPrepared iSource, sr, iConfig, kControl, kRun')
            csd = f"""<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr = {args.sample_rate}
ksmps = {args.block}
nchnls = 2
0dbfs = 1
#include "{ROOT / 'include/naviergrain.inc'}"
#include "{ROOT / 'include/naviergrain-prepared.inc'}"
instr 1
iSource ftgen 0, 0, -997, 10, 1, .2, .1
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_GRID_SIZE] = {grid}
iConfig[$NG_CONFIG_EMITTER_COUNT] = {emitters}
iConfig[$NG_CONFIG_MAX_GRAINS] = {voices}
iConfig[$NG_CONFIG_PRESSURE_ITERATIONS] = {pressure}
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_GRAIN_RATE] init {rate}
kControl[$NG_CONTROL_GRAIN_MS] init {grain_ms}
kControl[$NG_CONTROL_GAIN] init .03
{reader}
chnset kStats[$NG_STAT_STATUS], "status"
chnset kStats[$NG_STAT_SOURCE_LENGTH], "source_length"
chnset kStats[$NG_STAT_FIELD_AGE_MS], "age"
chnset kStats[$NG_STAT_SNAPSHOT_SEQUENCE], "sequence"
chnset kStats[$NG_STAT_LIVE_GRAINS], "live"
chnset kStats[$NG_STAT_NUMERIC_INTERVENTIONS], "numeric"
chnset kStats[$NG_STAT_VOICE_DROPS], "drops"
chnset kStats[$NG_STAT_KINETIC_ENERGY], "energy"
chnset kStats[$NG_STAT_RMS_DIVERGENCE], "divergence"
outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 0 {-1 if args.prepared else args.seconds}
{'f 0 z' if args.prepared else 'e'}
</CsScore>
</CsoundSynthesizer>
"""
            start = time.perf_counter_ns()
            if lib.csoundCompileCSD(host, csd.encode(), 1, 0) or lib.csoundStart(host):
                raise RuntimeError('Compile/start failed')
            compile_start_ms = (time.perf_counter_ns() - start) / 1e6
            preparation_ms = None
            if args.prepared:
                # No device/callback thread exists yet. One bounded host call
                # dispatches the persistent instrument and runs its init path.
                start = time.perf_counter_ns()
                status = lib.csoundPerformKsmps(host)
                preparation_ms = (time.perf_counter_ns() - start) / 1e6
                audio = lib.csoundGetSpout(host)
                if (status != 0 or channel(host, 'status') != 1 or
                        channel(host, 'source_length') != 997 or
                        any(channel(host, field) != 0 for field in ('live', 'sequence', 'age')) or
                        any(audio[i] != 0 for i in range(args.block * 2))):
                    raise RuntimeError('Silent preparation/ready handshake failed; do not start audio')
                lib.csoundSetControlChannel(host, b'run', 1)
            rss_prepared = resident_bytes()
            sequence = 0
            maximum_calls = math.ceil(args.seconds * args.sample_rate / args.block) + 2
            for _ in range(maximum_calls):
                if probe:
                    probe.ng_profile_begin()
                start = time.perf_counter_ns()
                status = lib.csoundPerformKsmps(host)
                elapsed = (time.perf_counter_ns() - start) / 1e6
                if probe:
                    probe.ng_profile_end(probe_values)
                    if probe_values[5]:
                        raise RuntimeError('Solver monotonic clock failed')
                    solver_steps += int(probe_values[0])
                    allocations += int(probe_values[3])
                    arena_bytes = int(probe_values[4])
                    maximum_step_ms = max(maximum_step_ms, probe_values[2] / 1e6)
                    if probe_values[0]:
                        solver_times.append(probe_values[1] / 1e6)
                if status < 0 or (args.prepared and status > 0):
                    raise RuntimeError(f'Performance failed: {status}')
                if status > 0 and len(times) * args.block >= args.seconds * args.sample_rate:
                    break
                times.append(elapsed)
                current = channel(host, 'sequence')
                if current != sequence:
                    fluid_times.append(elapsed)
                    energy = channel(host, 'energy')
                    divergence = channel(host, 'divergence')
                    if not math.isfinite(energy) or not math.isfinite(divergence) or energy < 0:
                        raise RuntimeError('Invalid published field diagnostics')
                    peak_energy = max(peak_energy, energy)
                    # Same dimensionless metric as the numerical/preset gates.
                    relative = divergence / (grid * max(math.sqrt(2 * energy), .001))
                    peak_relative_divergence = max(peak_relative_divergence, relative)
                sequence = current
                peak_live = max(peak_live, channel(host, 'live'))
                numeric = max(numeric, channel(host, 'numeric'))
                drops = max(drops, channel(host, 'drops'))
                audio = lib.csoundGetSpout(host)
                for i in range(args.block * 2):
                    sample = audio[i]
                    if not math.isfinite(sample):
                        raise RuntimeError('Non-finite un-limited audio')
                    peak_audio = max(peak_audio, abs(sample))
                audio_hash.update(C.string_at(audio, args.block * 2 * C.sizeof(C.c_double)))
                now = time.perf_counter()
                if now - last_progress >= args.progress_seconds:
                    progress = dict(profile=name, calls=len(times),
                                    rendered_seconds=len(times)*args.block/args.sample_rate,
                                    wall_seconds=now-profile_start, peak_live=peak_live,
                                    process_rss_bytes=resident_bytes(),
                                    numeric_interventions=numeric, voice_drops=drops)
                    report['progress'] = progress
                    write_report(args.output, report)
                    print(json.dumps({'progress': progress}), flush=True)
                    last_progress = now
                if args.prepared and len(times) * args.block >= args.seconds * args.sample_rate:
                    break
                if status > 0:
                    break
            else:
                raise RuntimeError('Score did not end')
            interval = 1000 * args.block / args.sample_rate
            rss_rendered = resident_bytes()
            record = dict(profile=name, seconds=args.seconds, sample_rate=args.sample_rate,
                          rendered_seconds=len(times)*args.block/args.sample_rate,
                          wall_seconds=time.perf_counter()-profile_start,
                          block=args.block, grid=grid, emitters=emitters, pool=voices,
                          rate=rate, grain_ms=grain_ms, pressure=pressure,
                          compile_start_ms=compile_start_ms, first_callback_ms=times[0],
                          preparation_callback_ms=preparation_ms,
                          preparation_mode='gated pre-roll' if args.prepared else 'score init in first callback',
                          all_callbacks=summary(times), after_first_callback=summary(times[1:]),
                          callbacks_containing_fluid_tick=summary(fluid_times),
                          solver_work_per_callback=summary(solver_times),
                          solver_steps=solver_steps if probe else None,
                          maximum_single_solver_step_ms=maximum_step_ms if probe else None,
                          owned_arena_bytes=arena_bytes if probe else None,
                          process_rss_before_host_bytes=rss_before,
                          process_rss_after_preparation_bytes=rss_prepared,
                          process_rss_after_render_bytes=rss_rendered,
                          timing_values_bytes=(len(times)+len(fluid_times)+len(solver_times))*8,
                          callback_interval_ms=interval,
                          offline_calls_exceeding_interval=sum(t > interval for t in times),
                          offline_calls_exceeding_quarter_interval=sum(t > interval*.25 for t in times),
                          maximum_callback_interval_fraction=max(times)/interval,
                          peak_live=peak_live, peak_audio=peak_audio,
                          audio_sha256=audio_hash.hexdigest(),
                          audio_hash_format='interleaved stereo, native-endian float64',
                          numeric_interventions=numeric, voice_drops=drops,
                          peak_relative_divergence=peak_relative_divergence,
                          peak_field_energy=peak_energy,
                          underruns=None, allocations=allocations if probe else None,
                          allocation_scope='Direct static-core references only; not Csound/libc internals' if probe else None)
            if probe and (solver_steps == 0 or not arena_bytes):
                raise RuntimeError('Profiling hooks did not observe solver steps and preparation size')
            if numeric or drops or allocations or not 0 < peak_audio < 1:
                record['checks_passed'] = False
                records.append(record)
                raise RuntimeError(f'Profile failed audio/numerical checks: {record}')
            record['checks_passed'] = True
            records.append(record)
            report.pop('progress', None)
            write_report(args.output, report)
            print(json.dumps(record), flush=True)
        except BaseException as error:
            report['status'] = 'failed'
            report['failure'] = dict(profile=name, error=str(error), completed_calls=len(times))
            write_report(args.output, report)
            raise
        finally:
            while lib.csoundGetMessageCnt(host):
                message = lib.csoundGetFirstMessage(host)
                if message:
                    log.append(message.decode(errors='replace'))
                lib.csoundPopFirstMessage(host)
            try:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.with_name(f'{args.output.stem}-{name}.log').write_text(''.join(log))
            finally:
                lib.csoundDestroyMessageBuffer(host)
                lib.csoundDestroy(host)
    report['status'] = 'complete'
    report['completed_utc'] = datetime.now(timezone.utc).isoformat()
    write_report(args.output, report)


if __name__ == '__main__':
    main()
