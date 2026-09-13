#!/usr/bin/env python3
"""Exercise the real loadable module, not a replacement host mock."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]


def orchestra(sr=48000, block=32, config='', control='', source='', duration=.08, start=0):
    return f'''<CsoundSynthesizer>
<CsOptions>
</CsOptions>
<CsInstruments>
sr = {sr}
ksmps = {block}
nchnls = 2
0dbfs = 1
#include "{ROOT / 'include/naviergrain.inc'}"
instr 1
iSource ftgen 0, 0, -997, 10, 1, .2, .1
{source}
iConfig[] fillarray $NG_CONFIG_DEFAULTS
iConfig[$NG_CONFIG_SOURCE_LOOP] = 1
iConfig[$NG_CONFIG_SEED] = 0
{config}
kControl[] fillarray $NG_CONTROL_DEFAULTS
kControl[$NG_CONTROL_SCHEDULER] init 0
kControl[$NG_CONTROL_MAPPING_MIX] init 0
{control}
aL, aR, kStats[] naviergrain iSource, sr, iConfig, kControl
printks "NG_STATS %d %d %d %d\\n", 0, lenarray(kStats), kStats[$NG_STAT_SOURCE_LENGTH], kStats[$NG_STAT_BACKEND], kStats[$NG_STAT_STATUS]
outs aL, aR
endin
</CsInstruments>
<CsScore>
i 1 {start} {duration}
e
</CsScore>
</CsoundSynthesizer>
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--csound', required=True)
    parser.add_argument('--module', required=True)
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='naviergrain-test-') as folder:
        tmp = Path(folder)

        def run(text, error=None):
            nonlocal count
            count += 1
            csd, output = tmp / 'test.csd', tmp / 'test.wav'
            csd.write_text(text)
            result = subprocess.run([args.csound, f'--opcode-lib={args.module}', '--sample-accurate',
                                     '-d', '-m0', '-W', '-s', '-o', str(output), str(csd)],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            if error:
                if result.returncode == 0 or error not in result.stdout:
                    raise AssertionError(result.stdout)
                return None
            if result.returncode != 0:
                raise AssertionError(result.stdout)
            if not re.search(r'NG_STATS 20 997 0 [0-9]+', result.stdout):
                raise AssertionError('array contract not observed: ' + result.stdout)
            with wave.open(str(output)) as audio:
                assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
                raw = audio.readframes(audio.getnframes())
                samples = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
            assert samples and max(map(abs, samples)) < 32767
            return samples

        for sr in (44100, 48000, 96000):
            reference = run(orchestra(sr, 1))
            assert any(reference)
            for block in (16, 32, 64, 37):
                actual = run(orchestra(sr, block))
                # Hosts may pad the terminal block; compare active audio and require zero padding.
                active = int(sr * .08) * 2
                assert actual[:active] == reference[:active], (sr, block, 'block-dependent audio')
                assert not any(actual[active:]), (sr, block, 'inactive tail not zero')
        # Exercise real fluid mappings across host blocks, with i-time controls.
        moving = 'kControl[$NG_CONTROL_MAPPING_MIX] init 1\nkControl[$NG_CONTROL_GRAIN_RATE] init 600'
        reference = run(orchestra(block=1, control=moving, duration=.6))
        for block in (32, 37, 64):
            actual = run(orchestra(block=block, control=moving, duration=.6))
            assert actual[:57600] == reference[:57600], 'fluid motion depends on host block size'
            assert not any(actual[57600:])
        still = run(orchestra(control=moving+'\nkControl[$NG_CONTROL_FREEZE] init 1', duration=.6))
        assert any(still) and still != reference, 'flow is inaudible, or freeze stopped playback'
        assert run(orchestra()) == run(orchestra()), 'seed zero is not deterministic'
        mutated = orchestra().replace('outs aL, aR', 'tablew 0, 17, iSource\nouts aL, aR')
        assert run(mutated) == run(orchestra()), 'source table was not copied'
        # Two simultaneous instances must own their pools and random streams independently.
        doubled = orchestra(control='kControl[$NG_CONTROL_GAIN] init .05').replace('i 1 0 0.08', 'i 1 0 0.08\ni 1 0 0.08')
        mixed = run(doubled)
        single = run(orchestra(control='kControl[$NG_CONTROL_GAIN] init .1'))
        assert mixed == single, 'instances share mutable state'

        frozen = run(orchestra(control='kControl[$NG_CONTROL_FREEZE] = 1'))
        assert frozen == run(orchestra()), 'freeze stopped scheduling/playback'
        assert not any(run(orchestra(control='kControl[$NG_CONTROL_GRAIN_RATE] = 0')))
        partial = run(orchestra(start=7 / 48000, duration=1001 / 48000))
        assert not any(partial[:14]) and not any(partial[2016:]), 'partial blocks leaked audio'
        run(orchestra(config='iConfig[$NG_CONFIG_BACKEND] = 1'), 'external backend needs a prepared instance ID')
        run(orchestra(config='iConfig[$NG_CONFIG_BACKEND] = 1\niConfig[$NG_CONFIG_INSTANCE_ID] = 1'),
            'external backend needs an unclaimed prepared provider')
        run(orchestra(config='iConfig[$NG_CONFIG_GRID_SIZE] = 24'), 'grid_size must be')
        run(orchestra(config='iConfig[$NG_CONFIG_MAX_GRAINS] = 1.5'), 'invalid range')
        run(orchestra(config='iConfig[] init 12'), 'expected 1D config[13]')
        run(orchestra(config='iConfig[][] init 13, 1'), 'expected 1D config[13]')
        run(orchestra(control='kControl[] init 23'), 'expected 1D config[13]')
        run(orchestra().replace('iSource, sr, iConfig', 'iSource, 0, iConfig'), 'source sample rate')
        run(orchestra(source='iSource = 999999'), 'source table is missing')
        run(orchestra(source='iSource = 1e30'), 'source table must be a positive integer')
        # GEN01 carries explicit channel metadata, unlike synthetic GEN tables.
        stereo = tmp / 'stereo.wav'
        with wave.open(str(stereo), 'wb') as audio:
            audio.setnchannels(2); audio.setsampwidth(2); audio.setframerate(48000)
            audio.writeframes(bytes(4000))
        run(orchestra(source=f'iSource ftgen 0, 0, 0, 1, "{stereo}", 0, 0, 0'), 'source table must be mono')
    print(f'Csound: {count} real plugin runs passed (ABI, rates/blocks, repeatability, offsets, malformed inputs)')


if __name__ == '__main__':
    main()
