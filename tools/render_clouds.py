#!/usr/bin/env python3
"""Render reproducible fluid sketches and fixed-mapping controls; stdlib only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('vortex-bloom', 'honey-drift', 'ion-blizzard', 'gravity-silk')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--csound', required=True, type=Path)
    parser.add_argument('--module', required=True, type=Path)
    parser.add_argument('--shuffle-module', type=Path,
                        help='Test-only plugin: also render raw spatial-shuffle comparisons')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/clouds')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    template = (ROOT / 'examples/fluid-clouds.csd').read_text()
    # Csound resolves nested includes relative to this absolute include root.
    template = template.replace('"../include/', f'"{ROOT}/include/')
    template = template.replace('"source.inc"', f'"{ROOT}/examples/source.inc"')
    template = template.replace('"fluid-presets.inc"', f'"{ROOT}/examples/fluid-presets.inc"')
    if args.shuffle_module:
        # These are diagnostic comparisons, with only identical outer fades.
        template = template.replace('outs tanh(aL)*aFade, tanh(aR)*aFade',
                                    'outs aL*aFade, aR*aFade')
    records = []
    comparisons = []
    variants = [(args.module, '', 1), (args.module, '-baseline', 0)]
    if args.shuffle_module:
        variants += [(args.shuffle_module, '-shuffled', 1),
                     (args.shuffle_module, '-shuffled-baseline', 0)]
    for scene, name in enumerate(NAMES, 1):
        pcm = {}
        for module, suffix, mix in variants:
            label = name + suffix
            output = (args.output / f'{label}.wav').resolve()
            csd = re.sub(r'<CsScore>.*?</CsScore>',
                         f'<CsScore>\ni 1 0 10 {scene} {mix}\ne\n</CsScore>', template, flags=re.S)
            with tempfile.TemporaryDirectory(prefix='naviergrain-cloud-') as folder:
                source = Path(folder) / 'cloud.csd'
                source.write_text(csd)
                try:
                    result = subprocess.run([str(args.csound.resolve()), f'--opcode-lib={module.resolve()}',
                                             '-W', '-s', '-o', str(output), str(source)],
                                            capture_output=True, text=True, timeout=300)
                except subprocess.TimeoutExpired as error:
                    def decoded(value):
                        return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
                    (args.output / f'{label}.log').write_text(
                        decoded(error.stdout) + decoded(error.stderr) + '\nRender timed out.\n')
                    raise
            log = result.stdout + result.stderr
            (args.output / f'{label}.log').write_text(log)
            if result.returncode:
                raise RuntimeError(log)
            observations = re.findall(r'NG_CLOUD (\d+) live=([\d.]+) peak=([\d.]+) divergence=([\d.]+) numeric=([\d.]+) drops=([\d.]+)', log)
            if not observations:
                raise RuntimeError('Missing opcode diagnostics: ' + log)
            _, live, peak, divergence, numeric, drops = observations[-1]
            with wave.open(str(output)) as audio:
                frames, sr = audio.getnframes(), audio.getframerate()
                assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
                pcm[suffix] = audio.readframes(frames)
                samples = struct.unpack('<'+'h'*(frames*2), pcm[suffix])
            record = dict(name=label, mapping_mix=mix, seconds=frames/sr, sample_rate=sr,
                          peak_pcm=max(map(abs,samples))/32768,
                          rms_pcm=(sum(x*x for x in samples)/len(samples))**.5/32768,
                          peak_live=int(float(live)), pre_limiter_peak=float(peak),
                          worst_relative_divergence=float(divergence),
                          numeric_interventions=int(float(numeric)), voice_drops=int(float(drops)))
            assert record['rms_pcm'] > 0 and record['peak_pcm'] < .999
            assert record['numeric_interventions'] == record['voice_drops'] == 0
            assert record['worst_relative_divergence'] <= .02
            if args.shuffle_module:
                assert record['pre_limiter_peak'] < .999, record
                record['spatial_shuffle'] = 'shuffled' in suffix
                record['host_limiter'] = False
                record['module_sha256'] = hashlib.sha256(module.read_bytes()).hexdigest()
                record['pcm_sha256'] = hashlib.sha256(pcm[suffix]).hexdigest()
            records.append(record)
            print(json.dumps(record), flush=True)
        if args.shuffle_module:
            assert pcm['-baseline'] == pcm['-shuffled-baseline'], 'Shuffle changed mapping_mix=0 audio'
            assert len(pcm['']) == len(pcm['-shuffled'])
            difference = sum((a[0] - b[0]) ** 2 for a, b in zip(
                struct.iter_unpack('<h', pcm['']), struct.iter_unpack('<h', pcm['-shuffled'])))
            rms_difference = (difference / (len(pcm['']) / 2)) ** .5 / 32768
            assert rms_difference > 1e-4, 'Shuffle comparison is inaudible or not enabled'
            with wave.open(str(args.output / f'{name}-spatial-ab.wav'), 'wb') as audio:
                audio.setnchannels(2)
                audio.setsampwidth(2)
                audio.setframerate(sr)
                audio.writeframes(pcm[''] + bytes(round(.25 * sr) * 4) + pcm['-shuffled'])
            comparisons.append(dict(name=name, fixed_mapping_pcm_identical=True,
                                    fluid_vs_shuffle_rms_difference=rms_difference,
                                    listening_order=['fluid', '250 ms silence', 'spatial shuffle']))
    (args.output / 'measurements.json').write_text(json.dumps(records, indent=2)+'\n')
    if args.shuffle_module:
        sources = ['src/naviergrain_core.c', 'tests/spatial_shuffle.inc',
                   'examples/fluid-clouds.csd', 'examples/fluid-presets.inc',
                   'examples/source.inc', 'tools/render_clouds.py']
        receipt = dict(scope='Offline spatial-correlation diagnostic; no limiter/effects',
                       csound_sha256=hashlib.sha256(args.csound.read_bytes()).hexdigest(),
                       source_sha256={p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources},
                       comparisons=comparisons, records=records)
        (args.output / 'spatial-comparisons.json').write_text(json.dumps(receipt, indent=2)+'\n')


if __name__ == '__main__':
    main()
