"""Contained dummy-audio/fake Last.fm HTTPS soak. Never touches personal config."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from lastfm_integration import Handler, certificate, environment


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, cert, key):
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(cert, key)
        self.lock = threading.Lock()
        self.errors = []
        self.count = 0  # Do not retain request history in the fixture.
        super().__init__(("127.0.0.1", 0), Handler)

    def reply(self, target, params):
        assert target == "/2.0/?format=json"
        assert params["api_key"] == "a"*32 and params["sk"] == "d"*32
        signing = "".join(k+params[k] for k in sorted(params) if k not in ("api_sig", "format", "callback"))+"b"*32
        assert params["api_sig"] == hashlib.md5(signing.encode()).hexdigest()
        with self.lock:
            self.count += 1
        if params["method"] == "track.updateNowPlaying":
            return {"nowplaying": {"ignoredMessage": {"code": "0"}}}
        assert params["method"] == "track.scrobble"
        return {"scrobbles": {"@attr": {"accepted": 1, "ignored": 0}, "scrobble": {
            "timestamp": params["timestamp"], "ignoredMessage": {"code": "0"}}}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=4500)
    parser.add_argument("--modes", nargs="+", default=["off", "fake", "https"])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lastfm-soak-", dir="/tmp/opencode") as temporary:
        root = Path(temporary)
        audio = root / "audio"
        audio.mkdir()
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                        str(audio / "sample.wav")], check=True)
        for suffix in ("mp3", "ogg"):
            subprocess.run(["ffmpeg", "-v", "error", "-i", str(audio / "sample.wav"), str(audio / f"sample.{suffix}")], check=True)
        cert, key = certificate(root, "trusted", "ws.audioscrobbler.com")
        with Server(cert, key) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for mode in args.modes:
                    private = root / mode
                    private.mkdir()
                    env = dict(os.environ, SDL_AUDIODRIVER="dummy") | environment(server, cert, private)
                    # A non-listening local proxy for non-HTTPS modes also prevents
                    # accidental use of inherited external proxy configuration.
                    with (args.output / f"{mode}.csv").open("w") as log, (args.output / f"{mode}.stderr").open("w") as errors:
                        subprocess.run([str(args.binary.resolve()), mode, str(args.cycles), str(private), str(audio)],
                                       env=env, stdout=log, stderr=errors, check=True, timeout=1200)
                    print(mode, "completed", args.cycles, "cycles", flush=True)
            finally:
                server.shutdown()
                thread.join(timeout=5)
            assert not server.errors, server.errors
            (args.output / "fixture.json").write_text(json.dumps({"cycles":args.cycles,"modes":args.modes,"https_requests":server.count,"errors":server.errors},indent=2)+"\n")


if __name__ == "__main__":
    main()
