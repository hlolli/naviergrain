"""Opt-in audio-device smoke. Defaults to ALSA null; select hardware explicitly."""
import argparse
import math
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def completed_audio(text):
    lines = [line for line in text.splitlines() if line.startswith("device=")]
    assert len(lines) == 1, text
    fields = dict(item.split("=", 1) for item in lines[0].split())
    assert float(fields["played"]) > 0, fields
    assert math.isfinite(float(fields["peak"])) and 0 < float(fields["peak"]) < 1, fields
    assert fields["cpu"] == "1", fields
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("module", type=Path)
    parser.add_argument("--backend", default="alsa")
    parser.add_argument("--output", default="dac:null")
    args = parser.parse_args()
    base = [str(args.host.resolve()), "--module", str(args.module.resolve())]
    device = base + ["--backend", args.backend, "--output", args.output]
    take = subprocess.run(device + ["--seconds", ".5", "--cpu-after", ".25"],
                          capture_output=True, text=True, timeout=30)
    assert take.returncode == 0, take.stdout + take.stderr
    fields = completed_audio(take.stdout)
    print(f"{args.backend}/{args.output}: generated audio, automatic or requested CPU fallback, and exact frame drain passed; GPU observed:", fields["gpu"])

    # Wait for actual device initialization; never signal a partially prepared host.
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen(device + ["--seconds", "30", "--cpu"],
                                   stdout=subprocess.PIPE, stderr=log, text=True)
        try:
            deadline = time.monotonic() + 20
            while True:
                log.seek(0)
                messages = log.read()
                if "Device ready;" in messages:
                    break
                assert process.poll() is None, messages
                assert time.monotonic() < deadline, "device preparation timed out"
                time.sleep(.01)
            time.sleep(.1)
            process.send_signal(signal.SIGINT)
            output, _ = process.communicate(timeout=10)
            assert process.returncode == 0, output
            completed_audio(output)
            print("Ctrl-C: stop submission, drain every queued source frame, close device and join worker passed")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    failure = subprocess.run(base + ["--backend", args.backend,
                                    "--output", "dac:naviergrain-missing-device", "--cpu"],
                             capture_output=True, text=True, timeout=20)
    assert failure.returncode == 1, failure.stdout + failure.stderr
    assert "Cannot open device" in failure.stderr, failure.stderr
    assert "device=" not in failure.stdout, failure.stdout
    print("failed device open: clean nonzero exit without a success receipt passed")


if __name__ == "__main__":
    main()
