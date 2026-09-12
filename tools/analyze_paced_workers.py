#!/usr/bin/env python3
"""Verify complete paced-worker receipts and retained PCM; no live-audio verdict."""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import sys


def check(condition, message):
    if not condition:
        raise ValueError(message)


def pcm_metrics(path, artifact, frames):
    data = path.read_bytes()
    check(len(data) == frames * 16 == artifact['bytes'], 'PCM length mismatch')
    check(hashlib.sha256(data).hexdigest() == artifact['sha256'], 'PCM hash mismatch')
    samples = array('d')
    samples.frombytes(data)
    if sys.byteorder != 'little':
        samples.byteswap()
    check(all(math.isfinite(v) for v in samples), 'Non-finite PCM')
    peak = max(map(abs, samples))
    check(peak > .0001, 'Silent PCM')
    return {'peak': peak, 'rms': math.sqrt(sum(v*v for v in samples)/len(samples)),
            'clippedSamples': sum(abs(v) >= 1 for v in samples)}


def analyze(directory):
    receipt = json.loads((directory / 'receipt.json').read_text())
    check(receipt['status'] == 'complete', 'Refusing incomplete measurement')
    expected = {('external','default','none'), ('gpu','default','none'),
                ('external','swirl','none'), ('gpu','swirl','none'),
                ('external','default','stall'), ('gpu','default','failure')}
    records = receipt['records']
    check(len(records) == 6 and {(r['mode'],r['profile'],r['fault']) for r in records} == expected,
          'Missing or duplicate workload')
    rows = []
    for r in records:
        audio, provider = r['audio'], r['provider']
        seconds = r['seconds']
        check(seconds == receipt['seconds'] and audio['frames'] == seconds * 48000, 'Incomplete render')
        check(audio['callbackMs']['count'] == seconds * 750, 'Incorrect callback count')
        check(audio['underruns'] is None, 'Timer runs cannot measure underruns')
        check(all(t['wallMs'] + 256/48 >= t['second']*1000 for t in audio['telemetry']),
              'Audio clock outran the no-catch-up sink')
        check(audio['heapUnchanged'] and provider['heapUnchanged'], 'Heap changed')
        check([t['second'] for t in audio['telemetry']] == list(range(1, seconds+1)), 'Missing telemetry')
        check(not audio['stats']['voice_drops'] and not audio['stats']['numeric_interventions'], 'Safety failure')
        check(audio['stats']['field_age_ms'] < 100, 'Field delivery did not recover')
        if r['fault'] == 'stall':
            check(provider['injected'] and provider['recovered'] and audio['commands']['dropped'] > 0,
                  'Stall/recovery not exercised')
            check(audio['fieldAgeMs']['max'] >= 900, 'No aged field during stall')
            check(any(t['live_grains'] > 0 and t['field_age_ms'] >= 900 for t in audio['telemetry']),
                  'No evidence of grains continuing through stall')
        if r['mode'] == 'gpu':
            report = provider['report']
            check(report['gpuFields'] > 5 and not report['rejected'], 'No valid GPU fields')
            if r['fault'] == 'failure':
                check(provider['injected'] and report['cpuFields'] > 5 and report['waitingEpoch'] is None
                      and audio['stats']['epoch'] >= 2, 'No completed audio-owned fallback epoch')
            else:
                check(report['backend'] == 'gpu' and report['cpuFields'] == 0, 'Unexpected GPU substitution')
        name = Path(r['artifact']['file'])
        check(name.name == str(name), 'Expected local PCM filename')
        metrics = pcm_metrics(directory / name, r['artifact'], audio['frames'])
        rows.append({'mode': r['mode'], 'profile': r['profile'], 'fault': r['fault'],
                     'wallSeconds': audio['elapsedMs']/1000, 'callbackMs': audio['callbackMs'],
                     'timerLatenessMs': audio['timerLatenessMs'], 'fieldAgeMs': audio['fieldAgeMs'],
                     'callbacksOverNominalPeriod': audio['callbacksOverNominalPeriod'],
                     'missedBatches': audio['missedBatches'], 'commands': audio['commands'],
                     'fields': audio['fields'], 'maximumLive': audio['maximumLive'],
                     'provider': provider.get('report'), 'pcm': metrics})
    return {'status':'complete','scope':receipt['scope'], 'browser':receipt['browser'],
            'seconds':receipt['seconds'],'rows':rows,'underruns':None,
            'receiptSha256':hashlib.sha256((directory/'receipt.json').read_bytes()).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    with (args.directory/'analysis.json').open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(args.directory/'analysis.json')
