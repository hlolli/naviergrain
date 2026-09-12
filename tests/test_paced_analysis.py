"""Independent artifact guards: partial results and corrupt PCM must not pass."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).parents[1]/'tools/analyze_paced_workers.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class Artifacts(unittest.TestCase):
    def test_pcm_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'audio.f64le'
            data = struct.pack('<4d', .25, -.25, .5, -.5)
            path.write_bytes(data)
            artifact = {'bytes':len(data), 'sha256':hashlib.sha256(data).hexdigest()}
            result = analysis.pcm_metrics(path, artifact, 2)
            self.assertEqual(result['peak'], .5)
            self.assertEqual(result['clippedSamples'], 0)
            self.assertAlmostEqual(result['rms'], (0.15625)**.5)
            path.write_bytes(data[:-1])
            with self.assertRaisesRegex(ValueError, 'length'):
                analysis.pcm_metrics(path, artifact, 2)
            path.write_bytes(data[:-1]+b'\0')
            with self.assertRaisesRegex(ValueError, 'hash'):
                analysis.pcm_metrics(path, artifact, 2)
            data = struct.pack('<4d', float('nan'), .5, .25, -.25)
            path.write_bytes(data)
            artifact['sha256'] = hashlib.sha256(data).hexdigest()
            with self.assertRaisesRegex(ValueError, 'Non-finite'):
                analysis.pcm_metrics(path, artifact, 2)

    def test_refuse_partial_or_missing_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'receipt.json'
            for status in ['running','failed']:
                path.write_text(json.dumps({'status':status}))
                with self.assertRaisesRegex(ValueError, 'incomplete'):
                    analysis.analyze(Path(directory))
            path.write_text(json.dumps({'status':'complete','records':[]}))
            with self.assertRaisesRegex(ValueError, 'Missing'):
                analysis.analyze(Path(directory))


if __name__ == '__main__':
    unittest.main()
