#!/usr/bin/env python3
"""Serve the generated workbench locally, optionally enabling its SAB bridge."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

class Handler(SimpleHTTPRequestHandler):
    isolated = False

    def end_headers(self):
        if self.isolated:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--directory", type=Path, default=Path("build/web"))
    parser.add_argument("--isolated", action="store_true")
    args = parser.parse_args()
    Handler.isolated = args.isolated
    server = ThreadingHTTPServer(("127.0.0.1", args.port),
        partial(Handler, directory=str(args.directory.resolve())))
    print(f"http://127.0.0.1:{args.port} — isolation={args.isolated}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
