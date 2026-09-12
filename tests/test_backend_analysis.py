"""Numerical oracle checks for offline audio metrics, not DSP implementation tests."""
import importlib.util
from pathlib import Path
import unittest
import json
import tempfile
import numpy as np

spec = importlib.util.spec_from_file_location("analyzer", Path(__file__).parents[1] / "tools/analyze_backends.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class AnalysisTest(unittest.TestCase):
    def setUp(self):
        time = np.arange(48000) / 48000
        self.tone = np.sin(2 * np.pi * 1500 * time) * 0.25
        self.stereo = np.column_stack((self.tone, self.tone))

    def test_known_sine(self):
        report = analyzer.describe(self.stereo)
        self.assertAlmostEqual(report["rms"], .25 / np.sqrt(2), places=12)
        self.assertAlmostEqual(report["centroidHz"], 1500, places=3)
        self.assertAlmostEqual(report["crestDb"], 10 * np.log10(2), places=10)
        self.assertAlmostEqual(sum(report["bandPowerFractions"]), 1, places=12)

    def test_antiphase_does_not_cancel_spectrum(self):
        inverted = np.column_stack((self.tone, -self.tone))
        a, b = analyzer.describe(self.stereo), analyzer.describe(inverted)
        self.assertEqual(a["bandPowerFractions"], b["bandPowerFractions"])
        self.assertAlmostEqual(b["stereoCorrelation"], -1, places=12)

    def test_gain_and_frequency_changes_are_not_hidden(self):
        gain = analyzer.compare(self.stereo, self.stereo * 2)
        self.assertAlmostEqual(gain["rmsDeltaDb"], 20 * np.log10(2), places=10)
        self.assertEqual(gain["spectralTotalVariation"], 0)
        self.assertFalse(gain["screenWithinBounds"])
        high = np.sin(2 * np.pi * 6000 * np.arange(48000) / 48000) * .25
        shifted = analyzer.compare(self.stereo, np.column_stack((high, high)))
        self.assertGreater(shifted["spectralTotalVariation"], .99)
        self.assertFalse(shifted["screenWithinBounds"])

    def test_identity_and_rejections(self):
        report = analyzer.compare(self.stereo, self.stereo)
        self.assertEqual(report["maxSampleDifference"], 0)
        self.assertTrue(report["screenWithinBounds"])
        for bad in (np.zeros_like(self.stereo), np.full_like(self.stereo, np.nan), self.stereo[:64], self.tone):
            with self.assertRaises(ValueError):
                analyzer.describe(bad)

    def test_partial_and_tampered_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / "receipt.json"
            receipt.write_text(json.dumps({"status": "running"}))
            with self.assertRaisesRegex(ValueError, "partial"):
                analyzer.analyze(root)
            records = [{"mode": mode, "profile": profile, "seconds": 1,
                        "artifact": {"file": "signal.f64le", "bytes": 3, "sha256": "wrong"}}
                       for mode in ("internal", "external", "gpu")
                       for profile in ("fixed", "default", "swirl")]
            receipt.write_text(json.dumps({"status": "complete", "records": records}))
            (root / "signal.f64le").write_bytes(b"bad")
            with self.assertRaisesRegex(ValueError, "checksum"):
                analyzer.analyze(root)


if __name__ == "__main__":
    unittest.main()
