"""Real libcurl/TUI against an isolated TLS proxy; never contacts Last.fm."""

import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time
from urllib.parse import parse_qs
import wave

from tui_integration import Session, write_library


KEY, SECRET, TOKEN, SESSION_KEY = (character * 32 for character in "abcd")


def certificate(root, name, hostname, openssl="openssl"):
    cert, key = root / f"{name}.crt", root / f"{name}.key"
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-days", "1", "-subj", f"/CN={hostname}", "-addext", f"subjectAltName=DNS:{hostname}",
                    "-keyout", str(key), "-out", str(cert)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
    return cert, key


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, cert, key):
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.lock = threading.Lock()
        self.calls, self.errors = [], []
        self.authorized = False
        super().__init__(("127.0.0.1", 0), Handler)

    def parameters(self, method):
        with self.lock:
            return [dict(params) for params in self.calls if params["method"] == method]

    def reply(self, target, params):
        assert target == "/2.0/?format=json", "format must be negotiated in the URL"
        assert params["api_key"] == KEY and params["format"] == "json"
        signing = "".join(key + params[key] for key in sorted(params) if key not in ("api_sig", "format", "callback")) + SECRET
        assert params["api_sig"] == hashlib.md5(signing.encode()).hexdigest()
        assert "secret" not in params
        with self.lock:
            self.calls.append(dict(params))
        method = params["method"]
        if method == "auth.getToken":
            return {"token": TOKEN}
        if method == "auth.getSession":
            assert params["token"] == TOKEN
            if not self.authorized:
                return {"error": 14, "message": "fixture authorization pending"}
            return {"session": {"name": "tls-fixture-user", "key": SESSION_KEY}}
        assert params["sk"] == SESSION_KEY
        if method == "track.updateNowPlaying":
            return {"nowplaying": {"ignoredMessage": {"code": "0"}}}
        assert method == "track.scrobble"
        assert params["artist"] == "TLS fixture artist" and params["track"] == "TLS fixture 日本"
        assert params["duration"] == "32"
        assert set(params) == {"method", "artist", "track", "duration", "timestamp", "api_key", "api_sig", "format", "sk"}
        return {"scrobbles": {"@attr": {"accepted": 1, "ignored": 0}, "scrobble": {
            "timestamp": params["timestamp"], "ignoredMessage": {"code": "0"}}}}


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(5)
            # Read exactly to the CONNECT header boundary, not into TLS data.
            header = bytearray()
            while not header.endswith(b"\r\n\r\n"):
                byte = self.request.recv(1)
                if not byte:
                    return
                header.extend(byte)
                assert len(header) <= 8192
            assert bytes(header).split(b"\r\n", 1)[0] == b"CONNECT ws.audioscrobbler.com:443 HTTP/1.1"
            self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            try:
                tls = self.server.context.wrap_socket(self.request, server_side=True)
            except ssl.SSLError:
                return  # Expected for untrusted / hostname-mismatched fixtures.
            with tls, tls.makefile("rb") as stream:
                request = stream.readline(8192)
                if not request:
                    return  # TLS 1.3 peers can reject trust after our handshake completes.
                method, target, protocol = request.decode("ascii").strip().split(" ")
                assert method == "POST" and protocol == "HTTP/1.1"
                headers = {}
                for _ in range(64):
                    line = stream.readline(8192)
                    if line == b"\r\n":
                        break
                    name, value = line.decode("ascii").split(":", 1)
                    headers[name.lower()] = value.strip()
                else:
                    raise AssertionError("oversized fixture headers")
                assert headers["host"] == "ws.audioscrobbler.com"
                count = int(headers["content-length"])
                assert 0 < count < 32768
                parsed = parse_qs(stream.read(count).decode("utf-8"), strict_parsing=True)
                assert all(len(value) == 1 for value in parsed.values())
                params = {name: value[0] for name, value in parsed.items()}
                body = json.dumps(self.server.reply(target, params)).encode()
                tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                            str(len(body)).encode() + b"\r\n\r\n" + body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Normal client cancellation during stop/quit.
        except Exception as error:
            with self.server.lock:
                self.server.errors.append(type(error).__name__ + ": " + str(error))


@contextlib.contextmanager
def proxy(cert, key):
    with Proxy(cert, key) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            yield server
        finally:
            server.shutdown()
            worker.join(timeout=5)
        assert not server.errors, server.errors


def environment(server, ca, config):
    return {"HTTPS_PROXY": f"http://127.0.0.1:{server.server_address[1]}",
            "https_proxy": f"http://127.0.0.1:{server.server_address[1]}",
            "HTTP_PROXY": "", "http_proxy": "", "ALL_PROXY": "", "all_proxy": "",
            "NO_PROXY": "", "no_proxy": "", "SSL_CERT_FILE": str(ca), "XDG_CONFIG_HOME": str(config)}


def setup(session):
    session.expect("State: stopped")
    session.send("l")
    session.expect("needs API credentials")
    session.send("\n" + KEY + "\n" + SECRET + "\n")


def exercise_lastfm_https(executable, root, openssl="openssl"):
    root.mkdir(parents=True)
    good_cert, good_key = certificate(root, "trusted", "ws.audioscrobbler.com", openssl)
    wrong_cert, wrong_key = certificate(root, "wrong-host", "not-lastfm.invalid", openssl)
    audio = root / "synthetic.wav"
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0" * 22050 * 32)
    library = root / "synthetic.ndjson"
    write_library(library, [{"type": "track", "stable_id": "tls-fixture", "beatmap_set_id": "tls-fixture",
                            "artist": "TLS fixture artist", "title": "TLS fixture 日本",
                            "audio_hash": hashlib.sha256(audio.read_bytes()).hexdigest(),
                            "audio_filename": "synthetic.wav", "audio_path": str(audio)}])
    config = root / "config"
    with proxy(good_cert, good_key) as server:
        with Session(executable, library, **environment(server, good_cert, config)) as session:
            setup(session)
            session.expect("authorize in browser, then Enter")
            session.send("\n")
            session.expect("not authorized yet")
            server.authorized = True
            session.send("\n")
            session.expect("connected")
            session.send("\n\n")  # Close panel, then play the fixture.
            session.expect("State: playing")
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline and not server.parameters("track.scrobble"):
                session.drain(0.1)
            assert len(server.parameters("track.scrobble")) == 1, "eligible fixture was not submitted via real HTTPS"
            session.send("l")
            session.expect("Scrobble accepted", "pending: 0")
            session.send("\x1bq")
            session.finish()
        assert len(server.parameters("track.updateNowPlaying")) == 1
        outbox = config / "osu-player/lastfm/scrobbles.json"
        stored = json.loads(outbox.read_text())
        assert stored["pending"] == [] and len(stored["receipts"]) == 1
        assert (outbox.stat().st_mode & 0o777) == 0o600
        # Private authorization restores without another token exchange or autoplay.
        with Session(executable, library, **environment(server, good_cert, config)) as session:
            session.expect("State: stopped", "Last.fm: connected")
            session.expect("Library (Enter plays")
            assert "Last.fm setup" not in session.screen.text(), "restoring authorization must not open setup"
            session.send("?")
            session.expect("Help", "State: stopped")
            session.send("\x1bl")
            session.expect("Last.fm setup", "connected")
            session.send("\x1bq")
            session.finish()
        assert len(server.parameters("auth.getToken")) == 1
        assert len(server.parameters("track.scrobble")) == 1
    for label, cert, key in (("untrusted", good_cert, good_key), ("hostname", wrong_cert, wrong_key)):
        with proxy(cert, key) as server:
            with Session(executable, library, **environment(server, wrong_cert, root / label)) as session:
                setup(session)
                session.expect("HTTPS: transfer failed (curl code 60)")
                session.send("\x1bq")
                session.finish()
            assert not server.calls, "TLS failure must happen before sending API credentials"
    print("Last.fm real-HTTPS tests passed: isolated proxy, certificate/hostname rejection, authorization, Now Playing, durable scrobble receipt and restart (no Last.fm contact)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default=str(Path(__file__).resolve().parent.parent / "bin/osu-player"))
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lastfm-https-", dir="/tmp/opencode" if Path("/tmp/opencode").is_dir() else None) as directory:
        exercise_lastfm_https(options.player_binary, Path(directory) / "fixture")


if __name__ == "__main__":
    main()
