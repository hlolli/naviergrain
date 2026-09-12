#!/usr/bin/env python3
"""Analyze retained binary64 stereo comparisons using NumPy's real FFT."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np

RATE = 48000
FFT_SIZE = 4096
BANDS = np.concatenate(([0.0], np.geomspace(20.0, 24000.0, 25)))
# Investigation thresholds, declared before the experiment. These are NOT
# universal perceptual equivalence or numerical stability certificates.
RMS_SCREEN_DB = 3.0
SPECTRAL_TV_SCREEN = 0.15


def describe(pcm):
    pcm = np.asarray(pcm, dtype=np.float64)
    if pcm.ndim != 2 or pcm.shape[1] != 2 or len(pcm) < FFT_SIZE or not np.isfinite(pcm).all():
        raise ValueError("Expected finite stereo PCM of at least one FFT window")
    rms = float(np.sqrt(np.mean(pcm * pcm)))
    if rms == 0:
        raise ValueError("Silent comparison signal")
    left, right = pcm.T
    correlation_denominator = np.sqrt(np.sum(left * left) * np.sum(right * right))
    window = np.hanning(FFT_SIZE)
    spectrum = np.zeros(FFT_SIZE // 2 + 1)
    count = 0
    # Average channel POWERS; downmixing would erase antiphase stereo.
    for start in range(0, len(pcm) - FFT_SIZE + 1, FFT_SIZE // 2):
        transformed = np.fft.rfft(pcm[start:start + FFT_SIZE] * window[:, None], axis=0)
        power = np.mean(np.abs(transformed) ** 2, axis=1)
        power[1:-1] *= 2
        spectrum += power
        count += 1
    spectrum /= count * RATE * np.sum(window * window)
    frequencies = np.fft.rfftfreq(FFT_SIZE, 1 / RATE)
    total = float(np.sum(spectrum))
    if total <= 0:
        raise ValueError("Zero spectral power")
    histogram, _ = np.histogram(frequencies, bins=BANDS, weights=spectrum)
    histogram /= np.sum(histogram)
    return {
        "frames": len(pcm), "rms": rms, "peak": float(np.max(np.abs(pcm))),
        "dc": np.mean(pcm, axis=0).tolist(),
        "crestDb": float(20 * np.log10(np.max(np.abs(pcm)) / rms)),
        "stereoCorrelation": float(np.sum(left * right) / correlation_denominator) if correlation_denominator else None,
        "centroidHz": float(np.sum(frequencies * spectrum) / total),
        "rolloff95Hz": float(frequencies[np.searchsorted(np.cumsum(spectrum), 0.95 * total)]),
        "bandPowerFractions": histogram.tolist(), "welchWindows": count,
    }


def compare(reference, candidate):
    if reference.shape != candidate.shape:
        raise ValueError("Mismatched PCM dimensions")
    a, b = describe(reference), describe(candidate)
    rms_db = float(20 * np.log10(b["rms"] / a["rms"]))
    tv = float(np.sum(np.abs(np.array(a["bandPowerFractions"]) - b["bandPowerFractions"])) / 2)
    return {
        "reference": a, "candidate": b,
        "rmsDeltaDb": rms_db, "spectralTotalVariation": tv,
        "relativeSampleRms": float(np.sqrt(np.mean((candidate - reference) ** 2)) / a["rms"]),
        "maxSampleDifference": float(np.max(np.abs(candidate - reference))),
        "screenWithinBounds": abs(rms_db) <= RMS_SCREEN_DB and tv <= SPECTRAL_TV_SCREEN,
    }


def analyze(directory):
    receipt_bytes = (directory / "receipt.json").read_bytes()
    receipt = json.loads(receipt_bytes)
    if receipt["status"] != "complete":
        raise ValueError("Refusing a failed or partial experiment")
    expected = {(mode, profile) for mode in ("internal", "external", "gpu")
                for profile in ("fixed", "default", "swirl")}
    if len(receipt["records"]) != 9 or {(r["mode"], r["profile"]) for r in receipt["records"]} != expected:
        raise ValueError("Incomplete comparison matrix")
    signals = {}
    for record in receipt["records"]:
        artifact = record["artifact"]
        path = Path(artifact["file"])
        if path.name != str(path):
            raise ValueError("PCM artifact must be a local basename")
        raw = (directory / path).read_bytes()
        if hashlib.sha256(raw).hexdigest() != artifact["sha256"] or len(raw) != artifact["bytes"]:
            raise ValueError("PCM checksum or length mismatch")
        pcm = np.frombuffer(raw, dtype="<f8")
        if len(pcm) != record["seconds"] * RATE * 2 or not np.isfinite(pcm).all():
            raise ValueError("PCM duration or finiteness mismatch")
        signals[record["mode"], record["profile"]] = pcm.reshape(-1, 2)
    fixed = signals["internal", "fixed"]
    if not all(np.array_equal(fixed, signals[mode, "fixed"]) for mode in ("external", "gpu")):
        raise ValueError("Fixed mappings changed PCM")
    pairs = []
    for profile in ("default", "swirl"):
        seconds = len(signals["external", profile]) // RATE
        intervals = [(0, 1), (1, min(10, seconds)), (10, seconds)]
        for start, end in intervals:
            if end <= start:
                continue
            for reference, candidate in (("external", "gpu"), ("internal", "external")):
                result = compare(signals[reference, profile][start * RATE:end * RATE],
                                 signals[candidate, profile][start * RATE:end * RATE])
                pairs.append({"profile": profile, "startSecond": start, "endSecond": end,
                              "referenceMode": reference, "candidateMode": candidate, **result})
    return {
        "status": "complete", "scope": receipt["scope"],
        "receiptSha256": hashlib.sha256(receipt_bytes).hexdigest(),
        "numpy": np.__version__, "fixedMappingExact": True,
        "method": {"sampleRate": RATE, "fftSize": FFT_SIZE, "hop": FFT_SIZE // 2,
                   "window": "symmetric Hann, no detrending, averaged stereo powers",
                   "bandEdgesHz": BANDS.tolist(),
                   "screenRmsDeltaDb": RMS_SCREEN_DB, "screenSpectralTotalVariation": SPECTRAL_TV_SCREEN,
                   "limits": "Descriptive single-seed screening, not perceptual equivalence, ensemble statistics, or live qualification"},
        "pairs": pairs,
        "allScreensWithinBounds": all(pair["screenWithinBounds"] for pair in pairs),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    output = args.directory / "analysis.json"
    with output.open("x") as file:
        json.dump(result, file, indent=2, allow_nan=False)
        file.write("\n")
    print(json.dumps({"output": str(output), "pairs": len(result["pairs"]),
                      "allScreensWithinBounds": result["allScreensWithinBounds"]}))


if __name__ == "__main__":
    main()
