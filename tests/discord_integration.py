"""Fake local Discord IPC + isolated PTY tests. Never contacts real services or libraries."""

import argparse
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
import wave

from tui_integration import Session, write_library


def read_exact(client, size):
    result = b""
    while len(result) < size:
        data = client.recv(size - len(result))
        if not data:
            raise EOFError("IPC closed")
        result += data
    return result


def frame(client):
    opcode, size = struct.unpack("<II", read_exact(client, 8))
    assert size <= 65536
    return opcode, read_exact(client, size)


def send(client, opcode, payload, fragmented=False):
    body = payload if isinstance(payload, bytes) else json.dumps(payload, ensure_ascii=False).encode()
    data = struct.pack("<II", opcode, len(body)) + body
    if fragmented:
        client.sendall(data[:3])
        client.sendall(data[3:9])
        client.sendall(data[9:])
    else:
        client.sendall(data)


def ack(client, request):
    send(client, 1, {"cmd": "SET_ACTIVITY", "nonce": request["nonce"], "data": None})


def activity(client):
    opcode, body = frame(client)
    assert opcode == 1
    request = json.loads(body)
    assert request["cmd"] == "SET_ACTIVITY" and request["args"]["pid"] > 0
    ack(client, request)
    return request["args"]["activity"]


class Server:
    def __init__(self, path):
        self.path = path
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.bind(str(path))
        self.socket.listen(8)
        self.socket.settimeout(4)

    def accept(self, ready=True):
        client, _ = self.socket.accept()
        client.settimeout(4)
        opcode, body = frame(client)
        assert opcode == 0 and json.loads(body) == {"v": 1, "client_id": "123456789012345678"}
        if ready:
            send(client, 3, b"handshake-ping", fragmented=True)
            assert frame(client) == (4, b"handshake-ping")
            send(client, 1, {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1}}, fragmented=True)
        return client

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *error):
        self.close()


class Driver:
    def __init__(self, binary, path):
        self.process = subprocess.Popen([binary, "--worker", str(path)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        assert self.line() == "ready"

    def line(self):
        assert select.select([self.process.stdout], [], [], 4)[0], "worker harness hung"
        result = self.process.stdout.readline()
        assert result, self.process.stderr.read()
        return result.strip()

    def command(self, value):
        self.process.stdin.write(value + "\n")
        self.process.stdin.flush()
        return self.line()

    def __enter__(self):
        return self

    def __exit__(self, *error):
        try:
            if self.process.poll() is None:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
                self.process.wait(timeout=2)
            diagnostics = self.process.stderr.read()
            assert self.process.returncode == 0 and not diagnostics, diagnostics
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=2)
            self.process.stdin.close()
            self.process.stdout.close()
            self.process.stderr.close()


def no_frame(client, duration=0.12):
    assert not select.select([client], [], [], duration)[0], "unexpected RPC traffic"


def no_connection(server, duration=0.12):
    assert not select.select([server.socket], [], [], duration)[0], "disabled/invalid client connected"


def idle_sample(driver, client, duration=0.6):
    """Linux per-thread counters: distinguish real idle blocking from timer polling."""
    def counters():
        result = {}
        for task in Path(f"/proc/{driver.process.pid}/task").iterdir():
            status = dict(line.split(":", 1) for line in (task / "status").read_text().splitlines())
            result[task.name] = (int(status["voluntary_ctxt_switches"]),
                                 int((task / "schedstat").read_text().split()[0]))
        return result

    before = counters()
    no_frame(client, duration)
    after = counters()
    wakeups = sum(after[task][0] - values[0] for task, values in before.items())
    cpu_ms = sum(after[task][1] - values[1] for task, values in before.items()) / 1_000_000
    print(f"Discord idle probe: {wakeups} voluntary context switches, {cpu_ms:.3f} ms CPU over {duration}s")
    return wakeups


def worker_tests(binary, root):
    path = root / "ipc"
    with Server(path) as server, Driver(binary, path) as driver:
        no_connection(server)
        assert driver.command("missing") == "needs app ID"
        no_connection(server)
        assert driver.command("invalid") == "invalid app ID"
        no_connection(server)
        driver.command("enable")
        with server.accept() as client:
            assert activity(client) is None
            driver.command("play")
            playing = activity(client)
            assert playing["type"] == 2 and playing["state"] == "作曲家" and "timestamps" in playing
            assert "details" not in playing
            assert playing["name"] == "日本語"
            assert playing["buttons"][0]["url"] == "https://osu.ppy.sh/beatmapsets/123"
            assert playing["assets"]["large_image"] == "https://assets.ppy.sh/beatmaps/123/covers/list@2x.jpg"
            assert playing["assets"]["small_image"] == "osu_player"
            assert playing["assets"]["large_text"] == "osu! player"
            for _ in range(30):
                driver.command("progress")
            no_frame(client, 0.25)  # Progress does not turn into periodic SET_ACTIVITY spam.
            send(client, 3, b"idle-ping", fragmented=True)
            assert frame(client) == (4, b"idle-ping")
            no_frame(client, 0.35)  # Idle ping/pong must not trigger an ACK/READY timeout.
            assert idle_sample(driver, client) < 8, "idle worker must block, not wake every 25 ms"
            driver.command("pause")
            paused = activity(client)
            assert paused["state"].startswith("Paused") and "timestamps" not in paused
            driver.command("seek")
            assert "timestamps" not in activity(client)
            driver.command("resume")
            resumed = activity(client)
            assert "timestamps" in resumed and resumed["state"] == "作曲家"
            assert abs(resumed["timestamps"]["start"] - (time.time() - 50)) < 3, "seek/resume must re-anchor the timer"
            driver.command("seek")
            for _ in range(3):
                driver.command("progress")
            coalesced = activity(client)
            assert abs(coalesced["timestamps"]["start"] - (time.time() - 53)) < 2, "throttled send must use the newest scalar position"
            driver.command("overflow")
            assert activity(client)["state"] == "作曲家"
            # Metadata replacement alone must update art, including while paused.
            driver.command("pause")
            assert "timestamps" not in activity(client)
            driver.command("map-change")
            changed = activity(client)
            assert changed["assets"]["large_image"] == "https://assets.ppy.sh/beatmaps/456/covers/list@2x.jpg"
            assert changed["buttons"][0]["url"].endswith("/456")
            assert changed["state"].startswith("Paused")
            driver.command("title-change")
            renamed = activity(client)
            assert renamed["name"] == "別の曲" and renamed["state"] == "Paused — 作曲家"
            assert "details" not in renamed and renamed["assets"]["large_text"] == "osu! player"
            driver.command("local-map")
            local = activity(client)
            assert local["assets"]["large_image"] == "osu_player"
            assert "small_image" not in local["assets"] and "buttons" not in local
            driver.command("play")
            assert activity(client)["assets"]["large_image"].endswith("/123/covers/list@2x.jpg")
            driver.command("stop")
            assert activity(client) is None
            driver.command("disable")
            # Best-effort clear is sent before disconnect, without requiring an ACK.
            opcode, body = frame(client)
            assert opcode == 1 and json.loads(body)["args"]["activity"] is None
        no_connection(server)
        driver.command("play")
        driver.command("enable")
        with server.accept() as client:
            assert activity(client)["state"] == "作曲家", "re-enable must restore latest state"
        # Peer disconnect should reconnect and publish only the newest state.
        driver.command("pause")
        with server.accept() as client:
            assert activity(client)["state"].startswith("Paused")
            # Neither a malformed frame nor a valid JSON error can escape to stderr/UI.
            send(client, 1, {"evt": "ERROR", "data": {"message": "UNTRUSTED SECRET"}})
        with server.accept() as client:
            assert activity(client)["state"].startswith("Paused")
            client.sendall(struct.pack("<II", 1, 65537))
        with server.accept() as client:
            assert activity(client)["state"].startswith("Paused")
            send(client, 1, b"{invalid json")
        with server.accept() as client:
            assert activity(client)["state"].startswith("Paused")
            # Slow/fragmented frame cannot hang configuration changes.
            client.sendall(b"\x01\x00")
            start = time.monotonic()
            assert driver.command("disable") == "off"
            assert time.monotonic() - start < 0.5
        no_connection(server)
    path.unlink()

    # Reconnect after missing socket, stalled READY, and missing activity ACK.
    with Driver(binary, path) as driver:
        driver.command("play")
        driver.command("enable")
        with Server(path) as server:
            with server.accept(ready=False):
                pass  # Closing before READY must remain recoverable.
            with server.accept(ready=False) as client:
                # Leave the peer waiting for READY until its deadline expires.
                client.settimeout(2)
                assert client.recv(1) == b""
            with server.accept() as client:
                opcode, body = frame(client)
                assert opcode == 1 and json.loads(body)["args"]["activity"] is not None
                # Do not ACK: the worker eventually closes instead of queueing indefinitely.
                frame(client)  # Best-effort clear after timeout.
            with server.accept() as client:
                assert activity(client)["state"] == "作曲家"
                driver.command("disable")
    path.unlink()

    # Normal destruction clears activity and joins even with an active session.
    with Server(path) as server:
        with Driver(binary, path) as driver:
            driver.command("play")
            driver.command("enable")
            client = server.accept()
            assert activity(client)["state"] == "作曲家"
        with client:
            opcode, body = frame(client)
            assert opcode == 1 and json.loads(body)["args"]["activity"] is None
            try:
                assert client.recv(1) == b""
            except ConnectionResetError:
                # Shutdown can cancel ACK consumption; closing a Unix socket
                # with that unread ACK may reset rather than deliver EOF.
                pass
    path.unlink()


def tui_tests(binary, root):
    path = root / "tui-ipc"
    config = root / "config/osu-player/settings.json"
    config.parent.mkdir(parents=True)
    config.write_text(json.dumps({"version": 1, "volume": 0.4, "discord": {
        "enabled": True, "application_id": "123456789012345678"}}))
    audio = root / "audio.wav"
    with wave.open(str(audio), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(8000)
        output.writeframes(b"\0\0" * 8000 * 30)
    library = root / "library.ndjson"
    write_library(library, [{"type": "track", "stable_id": "one", "beatmap_set_id": "one",
                            "title": "Title", "artist": "Artist", "audio_filename": "audio.wav",
                            "audio_hash": "a" * 64, "audio_path": str(audio)}])
    environment = {"XDG_CONFIG_HOME": str(config.parent.parent), "OSU_PLAYER_DISCORD_SOCKET": str(path)}
    with Server(path) as server:
        with Session(binary, library, **environment) as session:
            with server.accept() as client:
                assert activity(client) is None
                session.expect("Discord: connected", "Volume: 40%", "Library: 1")
                session.send("\n")
                session.expect("State: playing")
                client.settimeout(7)  # Production updates are throttled to five seconds.
                current = activity(client)
                assert current["state"] == "Artist" and current["name"] == "Title"
                session.send("d")
                session.expect("Discord: off", "State: playing")
                opcode, body = frame(client)
                assert opcode == 1 and json.loads(body)["args"]["activity"] is None
                saved = json.loads(config.read_text())
                assert saved["discord"] == {"enabled": False, "application_id": "123456789012345678"}
                assert abs(saved["volume"] - 0.4) < 0.001
                session.send("q")
                session.finish()
        with Session(binary, library, **environment) as session:
            session.expect("Discord: off", "Volume: 40%", "Library: 1")
            no_connection(server)
            session.send("q")
            session.finish()
    path.unlink()

    # Missing IDs prompt without connecting or mutating settings until accepted.
    config.write_text('{"version":1}')
    with Server(path) as server:
        with Session(binary, library, **environment) as session:
            session.expect("Discord: off", "Library: 1")
            session.send("/Title\n\n")
            session.expect("State: playing", "Search: Title")
            session.send("d")
            session.expect("Discord Application ID:", "Enter save & enable", "developers/applications")
            no_connection(server)
            session.send("\n")
            session.expect("Invalid Application ID")
            session.send("bad-id\n")
            session.expect("Invalid Application ID", "Discord Application ID: bad-id")
            session.resize(10, 45)
            session.expect("Terminal too small", "Esc cancels setup")
            session.resize(24, 100)
            session.expect("Discord Application ID: bad-id", "State: playing")
            no_connection(server)
            assert config.read_text() and not json.loads(config.read_text())["discord"]["enabled"]
            session.send("\x1b")
            session.expect("Search: Title", "Discord: off")
            assert "Discord Application ID:" not in session.screen.text()
            session.send("d")
            session.expect("Discord Application ID: _")
            # Overflow is rejected even if it has the right number of digits.
            session.send("18446744073709551616\n")
            session.expect("Invalid Application ID")
            # Ctrl-U clears; Backspace edits without affecting search/transport.
            session.send("\x15123456789012345679\x7f8")
            session.expect("Discord Application ID: 123456789012345678_")
            session.expect("State: playing", "00:01")
            no_connection(server)
            session.send("\n")
            with server.accept() as client:
                current = activity(client)
                assert current["state"] == "Artist" and current["name"] == "Title"
                session.expect("Discord: connected", "Search: Title", "State: playing")
                assert "Discord Application ID:" not in session.screen.text()
                saved = json.loads(config.read_text())
                assert saved["discord"] == {"enabled": True, "application_id": "123456789012345678"}
                assert saved["search"] == "Title"
                session.send("q")
                session.finish()
        with Session(binary, library, **environment) as session:
            with server.accept() as client:
                assert activity(client) is None
                session.expect("Discord: connected", "Search: Title")
                assert "Discord Application ID:" not in session.screen.text()
                session.send("q")
                session.finish()
    path.unlink()

    # Previously enabled configs with missing/invalid IDs prompt at startup too.
    for value in ("", "invalid"):
        config.write_text(json.dumps({"version": 1, "discord": {"enabled": True, "application_id": value}}))
        with Session(binary, library, **environment) as session:
            session.expect("Discord Application ID:", "Library: 1")
            session.send("\x1b")
            session.expect("Discord: off", "Search:")
            assert not json.loads(config.read_text())["discord"]["enabled"]
            session.send("q")
            session.finish()

    for invalid in ({"enabled": None}, {"enabled": "yes"}, {"application_id": None},
                    {"application_id": 123}, {"application_id": "x" * 33}, []):
        original = json.dumps({"version": 1, "discord": invalid})
        config.write_text(original)
        with Session(binary, library, **environment) as session:
            session.expect("invalid or unsupported settings file", "Library: 1")
            session.send("d")
            session.expect("Discord Application ID:")
            session.send("\x1b")
            session.send("q")
            session.finish()
        assert config.read_text() == original, "malformed Discord settings must be preserved"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", default="./bin/discord-presence-tests")
    parser.add_argument("--player-binary", default="./bin/osu-player")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="osu-discord-test-", dir="/tmp/opencode" if Path("/tmp/opencode").is_dir() else None) as temporary:
        root = Path(temporary)
        worker_tests(options.test_binary, root)
        tui_tests(options.player_binary, root)
    print("Discord fake-IPC tests passed: opt-in, framing, ping/pong, ACKs, throttling, reconnect, hostile/slow peers, settings, in-app ID setup and PTY responsiveness")


if __name__ == "__main__":
    main()
