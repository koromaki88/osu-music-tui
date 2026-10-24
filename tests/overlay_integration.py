"""Loopback OBS source: isolated settings, synthetic audio and real local sockets."""

import argparse
import contextlib
import hashlib
import http.client
import json
from pathlib import Path
import socket
import tempfile
import time
import wave

from tui_integration import Session, write_library

HOST = "127.0.0.1"
PORT = 7270


def request(path="/state", headers=None, method="GET"):
    connection = http.client.HTTPConnection(HOST, PORT, timeout=2)
    try:
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def until(predicate):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (ConnectionError, OSError, http.client.HTTPException):
            pass
        time.sleep(.05)
    raise AssertionError("overlay condition did not settle")


def state():
    code, headers, body = request()
    assert code == 200 and headers["Cache-Control"] == "no-store"
    assert "Access-Control-Allow-Origin" not in headers
    data = json.loads(body)
    assert set(data) == {"state", "title", "artist", "position_ms", "duration_ms", "thumbnail_url"}
    return data


def closed():
    try:
        connection = socket.create_connection((HOST, PORT), timeout=.2)
    except OSError:
        return True
    connection.close()
    return False


def exercise(executable, root):
    audio = root / "private-audio.wav"
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(22050)
        output.writeframes(b"\0\0" * 22050 * 60)
    title = '<img src=x onerror="alert(1)"> 日本'
    records = [{"type": "track", "stable_id": "private-id", "beatmap_set_id": "private-set",
                "online_set_id": 12345,
                "artist": "Overlay artist", "title": title, "audio_filename": "private-audio.wav",
                "audio_hash": hashlib.sha256(audio.read_bytes()).hexdigest(), "audio_path": str(audio)}]
    library = root / "library.ndjson"
    write_library(library, records)
    config = root / "config"
    settings = config / "osu-player/settings.json"
    style_path = config / "osu-player/overlay.json"
    # Occupy the fixed port before launching: default-off must neither bind nor
    # prevent playback. Never contact an unrelated server if port 7270 is in use.
    with socket.socket() as occupied:
        occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        occupied.bind((HOST, PORT))
        occupied.listen(1)
        with Session(executable, library, XDG_CONFIG_HOME=str(config)) as session:
            session.expect("Library: 1", "State: stopped")
            assert not style_path.exists(), "disabled overlay must not create style config"
            session.send("O")
            session.expect("cannot bind 127.0.0.1:7270")
            session.send("\n")
            session.expect("State: playing")
            occupied.close()
            session.send("O")
            session.expect("OBS overlay: off")
            session.send("O")
            session.expect("http://127.0.0.1:7270/overlay")
            until(lambda: state()["state"] == "playing")
            data = state()
            assert data["title"] == title and data["artist"] == "Overlay artist"
            assert data["thumbnail_url"] == "https://assets.ppy.sh/beatmaps/12345/covers/list@2x.jpg"
            assert str(audio) not in json.dumps(data) and "private-id" not in json.dumps(data)
            code, headers, html = request("/overlay")
            assert code == 200 and b"background:transparent" in html
            assert b"width:100%;min-height:180px" in html and b"max-width:800px" not in html, "default card must follow viewport width"
            assert b"c.width===null?'none'" in html, "fluid mode must clear a previously configured width cap"
            assert b"textContent=data.state" in html and b"innerHTML" not in html
            assert b"setTimeout(update,500)" in html and b"Player disconnected" in html
            assert "default-src 'none'" in headers["Content-Security-Policy"]
            assert "img-src https://assets.ppy.sh;" in headers["Content-Security-Policy"]
            assert b"object-fit:cover;flex:none" in html and b'img id="thumbnail"' in html
            assert b"thumbnail.onerror=" in html and b"updateCover(null)" in html
            assert request("/overlay?progress=0")[0] == 200
            def style():
                status, _, content = request("/style")
                assert status == 200
                return json.loads(content)
            until(lambda: style_path.exists() and style()["width"] is None)
            assert style_path.stat().st_mode & 0o777 == 0o600
            custom = {"width": 600, "height": 120, "font": "DejaVu Sans", "title_size": 26,
                      "artist_size": 18, "background": "#000000", "background_opacity": 0,
                      "text_color": "#ffcc00", "accent_color": "#00ff00", "corner_radius": 0,
                      "show_time": False, "show_progress": False, "show_thumbnail": False, "thumbnail_size": 96}
            style_path.write_text(json.dumps(custom))
            until(lambda: style()["width"] == 600)
            assert style() == custom
            until(lambda: state()["thumbnail_url"] is None)
            style_path.write_text('{"width":500,"font":"</script>"}')
            session.expect("OBS: style error")
            assert style() == custom, "invalid edit must preserve entire last valid style"
            assert style_path.read_text() == '{"width":500,"font":"</script>"}'
            style_path.unlink()
            until(lambda: style()["width"] is None)
            until(lambda: state()["thumbnail_url"] is not None)
            session.expect("OBS: on")
            style_path.write_text('{"width":640,"show_time":false}')
            until(lambda: style()["width"] == 640)
            for headers in ({"Host": "attacker.example:7270"}, {"Origin": "https://attacker.example"},
                            {"Origin": "null"}, {"Sec-Fetch-Site": "cross-site"}):
                assert request(headers=headers)[0] == 403
            assert request(headers={"Origin": "http://127.0.0.1:7270"})[0] == 200
            assert request("/../../etc/passwd")[0] == 404
            assert request(method="POST")[0] == 405
            for raw in (b"GET /state HTTP/1.1\r\n\r\n",
                        b"GET /state HTTP/1.1\r\nHost: 127.0.0.1:7270\r\nHost: attacker.example\r\n\r\n"):
                with socket.create_connection((HOST, PORT), timeout=2) as peer:
                    peer.sendall(raw)
                    assert peer.recv(1024).startswith(b"HTTP/1.1 403")
            with socket.create_connection((HOST, PORT), timeout=2) as peer:
                peer.sendall(b"GET /state HTTP/1.1\r\nX-Fill: " + b"x" * 5000)
                try:
                    assert peer.recv(1024) == b""
                except ConnectionResetError:
                    pass
            assert state()["state"] == "playing", "bad requests must not kill worker"
            session.send(" ")
            until(lambda: state()["state"] == "paused")
            before = state()["position_ms"]
            time.sleep(.7)
            assert state()["position_ms"] == before
            session.send("l")
            until(lambda: state()["position_ms"] >= before + 4500)
            session.send("x")
            until(lambda: state()["state"] == "idle")
            assert state()["title"] == "" and state()["artist"] == "" and state()["duration_ms"] is None
            assert state()["thumbnail_url"] is None
            # Slow readers/partial requests consume only bounded slots and cannot
            # hold shutdown or UI operations hostage.
            with contextlib.ExitStack() as stack:
                for _ in range(8):
                    peer = stack.enter_context(socket.create_connection((HOST, PORT), timeout=1))
                    peer.sendall(b"GET /state HTTP/1.1\r\nHost:")
                session.send("O")
                session.expect("OBS overlay: off")
                until(closed)
            session.send("O")
            until(lambda: state()["state"] == "idle")
            session.send("q")
            session.finish()
    until(closed)
    assert json.loads(settings.read_text())["overlay"]["enabled"] is True
    with Session(executable, library, XDG_CONFIG_HOME=str(config)) as session:
        session.expect("State: stopped", "OBS: on")
        until(lambda: state()["state"] == "idle")  # enablement, never track/autoplay
        until(lambda: json.loads(request("/style")[2])["width"] == 640)
        session.send("Oq")
        session.finish()
    until(closed)
    assert json.loads(settings.read_text())["overlay"]["enabled"] is False
    # Rejected settings must not silently enable the server or clobber the file.
    invalid = '{"version":1,"overlay":{"enabled":"yes"}}'
    settings.write_text(invalid)
    with Session(executable, library, XDG_CONFIG_HOME=str(config)) as session:
        session.expect("invalid or unsupported settings file", "State: stopped")
        assert closed()
        session.send("q")
        session.finish()
    assert settings.read_text() == invalid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player-binary", default=str(Path(__file__).resolve().parent.parent / "bin/osu-player"))
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-overlay-test-", dir="/tmp/opencode" if Path("/tmp/opencode").is_dir() else None) as directory:
        exercise(options.player_binary, Path(directory))
    print("OBS loopback overlay tests passed: opt-in, port conflict, HTTP boundaries, metadata privacy, live style config/rollback, playback/seek/idle, slow clients, shutdown and saved enablement (synthetic only)")


if __name__ == "__main__":
    main()
