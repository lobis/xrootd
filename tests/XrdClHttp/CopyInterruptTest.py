#!/usr/bin/env python3
"""Exercise xrdcp signal cleanup while HTTP COPY awaits response headers."""

import argparse
import contextlib
import http.server
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit

PAYLOAD = b"copy-interruption\0\xff" * 128


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def respond(self, status, payload=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if self.command != "HEAD":
            with contextlib.suppress(BrokenPipeError, ConnectionResetError):
                self.wfile.write(payload)

    def do_OPTIONS(self):
        self.respond(200)

    def do_HEAD(self):
        payload = self.server.files.get(urlsplit(self.path).path)
        self.respond(404 if payload is None else 200, payload or b"")

    def do_COPY(self):
        path = urlsplit(self.path).path
        if (self.server.mode == "pull" and path != "/target") or (
            self.server.mode == "push" and path != "/source"
        ):
            self.respond(400)
            return
        header = "Source" if self.server.mode == "pull" else "Destination"
        if not self.headers.get(header):
            self.respond(400)
            return
        self.server.files["/target"] = PAYLOAD[:len(PAYLOAD) // 2]
        self.server.ready.set()
        self.server.release.wait(15)
        self.close_connection = True

    def do_DELETE(self):
        self.server.files.pop(urlsplit(self.path).path, None)
        self.respond(204)


def check_case(xrdcp, environment, mode, sig, posc):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.mode = mode
    server.files = {"/source": PAYLOAD}
    server.ready = threading.Event()
    server.release = threading.Event()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    process = None
    try:
        base = "http://127.0.0.1:{}".format(server.server_port)
        arguments = [xrdcp, "--nopbar", "--tpc", "only", "--tpc-mode", mode]
        if posc:
            arguments.append("--posc")
        arguments.extend([
            base + "/source?xrdcl.http.noauth=true",
            base + "/target?xrdcl.http.noauth=true",
        ])
        process = subprocess.Popen(
            arguments, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, start_new_session=True,
        )
        assert server.ready.wait(10), "COPY did not create the partial target"
        process.send_signal(sig)
        _, stderr = process.communicate(timeout=8)
        assert process.returncode == 128 + sig, stderr.decode(errors="replace")
        expected = None if posc else PAYLOAD[:len(PAYLOAD) // 2]
        assert server.files.get("/target") == expected, "wrong partial cleanup"
        assert server.files["/source"] == PAYLOAD, "source modified"
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
        server.release.set()
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xrdcp", required=True)
    parser.add_argument("--plugin", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="xrd-signal-") as directory:
        config = Path(directory) / "http.conf"
        contents = "url = http://*\nlib = {}\nenable = true\n"
        config.write_text(contents.format(args.plugin))
        environment = dict(os.environ, XRD_PLUGINCONFDIR=directory,
                           XRD_CPTPCTIMEOUT="30", XRD_CPTIMEOUT="30")
        for mode in ("pull", "push"):
            for sig in (signal.SIGINT, signal.SIGTERM):
                for posc in (True, False):
                    check_case(args.xrdcp, environment, mode, sig, posc)
                    print(mode, sig.name, "posc=" + str(posc), "PASS")


if __name__ == "__main__":
    main()
