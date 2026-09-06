"""Serve the local flash utility and verified packages on loopback only."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

from firmware import BOARDS, ROOT, verify, read_json


class Handler(SimpleHTTPRequestHandler):
    verified_boards = set()

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        if path == "/":
            self.send_response(302)
            self.send_header("Location", "/docs/index.html")
            self.end_headers()
            return
        allowed = {"/docs/" + name for name in ("index.html", "main.js", "style.css", "favicon.svg")}
        for board in self.verified_boards:
            directory = ROOT / "dist" / board
            allowed.update(f"/dist/{board}/{name}" for name in read_json(directory / "checksums.json"))
            allowed.add(f"/dist/{board}/checksums.json")
        if path not in allowed:
            self.send_error(404, "Build and verify the selected board first")
            return
        super().do_GET()

    def do_HEAD(self):
        self.send_error(405)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8888)
    args = parser.parse_args()
    for board in BOARDS:
        try:
            verify(board, ROOT / "dist" / board)
            Handler.verified_boards.add(board)
        except (ValueError, OSError, KeyError) as exc:
            print(f"{board}: not available ({exc})")
    if not Handler.verified_boards:
        parser.error("Run python tools/firmware.py build --board <board> first")
    server = ThreadingHTTPServer(("127.0.0.1", args.port), partial(Handler, directory=str(ROOT)))
    print(f"Local flasher: http://127.0.0.1:{args.port}/docs/index.html", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
