#!/usr/bin/env python3
"""Validate an existing diagnostic render and its Csound log; stdlib only."""
import argparse
import json
from pathlib import Path
import re
import struct
import wave

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('viscosity', 'swirl', 'turbulence', 'strain_drive', 'inertia_ms', 'attraction')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audio', type=Path, default=ROOT / 'build/naviergrain-diagnostic.wav')
    parser.add_argument('--log', type=Path, default=ROOT / 'build/diagnostic.log')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/diagnostic-audio.json')
    args = parser.parse_args()
    schema = json.loads((ROOT / 'schema/naviergrain-v1.json').read_text())
    controls = {item['name']: index for index, item in enumerate(schema['control'])}
    log = args.log.read_text()
    assert 'Undefined macro' not in log, 'Score macros were not expanded'
    rows = re.findall(r'NG_DIAGNOSTIC control=(\d+) value=([\d.e+-]+) live=(\d+) peak=([\d.e+-]+) numeric=(\d+) drops=(\d+)', log)
    assert {int(r[0]) for r in rows} == {controls[name] for name in NAMES}
    assert all(int(r[4]) == 0 and int(r[5]) == 0 for r in rows)
    scenes = []
    with wave.open(str(args.audio)) as audio:
        sr, frames = audio.getframerate(), audio.getnframes()
        assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
        assert sr == 48000 and frames == 60 * sr
        for name in NAMES:
            raw = audio.readframes(10 * sr)
            samples = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
            peak = max(map(abs, samples)) / 32768
            rms = (sum(x*x for x in samples) / len(samples)) ** .5 / 32768
            assert 0 < peak < .999 and rms > .00001, f'Silent/clipped scene: {name}'
            observations = [row for row in rows if int(row[0]) == controls[name]]
            assert len(observations) >= 9
            assert len({float(row[1]) for row in observations}) >= 6, f'No sweep: {name}'
            scenes.append(dict(control=name, index=controls[name], seconds=10,
                               peak_pcm=peak, rms_pcm=rms,
                               peak_reported_pre_fade=max(float(r[3]) for r in observations),
                               peak_reported_live=max(int(r[2]) for r in observations)))
    record = dict(seconds=frames/sr, sample_rate=sr, limiter=False,
                  numeric_interventions=0, voice_drops=0, scenes=scenes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + '\n')
    print(f'Diagnostic: six audible sweeps, 60 seconds, no clipping/drops/interventions; {args.output}')


if __name__ == '__main__':
    main()
