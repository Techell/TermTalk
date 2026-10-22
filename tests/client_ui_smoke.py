"""Exercise RU/EN, Cyrillic messages, nonblocking input and persistent profile history."""
import argparse
import socket
import struct
import subprocess
import tempfile
from pathlib import Path
from wire import *


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return struct.pack("!I", value)


def journal(path):
    data = path.read_bytes()
    assert data[:4] == b"WSH1"
    assert data[-4:] == checksum(data[:-4])
    scope, nick, count, reserved = struct.unpack_from("!HHHH", data, 4)
    assert count <= 256 and not reserved
    at = 12 + scope + nick
    records = []
    for _ in range(count):
        outgoing, reserved, pn, tn, timestamp = struct.unpack_from("!BBHIQ", data, at)
        at += 16
        assert not reserved and timestamp > 0
        records.append((outgoing, data[at:at+pn].decode("utf-8"), data[at+pn:at+pn+tn].decode("utf-8")))
        at += pn + tn
    assert at == len(data) - 4
    return records


def session(executable, listener, directory, commands, texts=(), options=(), identifier=1, incoming=False, save_error=False):
    nick = "Алиса".encode()
    process = subprocess.Popen([str(executable), "127.0.0.1", str(listener.getsockname()[1]), "Алиса",
                                "--history-dir", str(directory), *options],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        peer, _ = listener.accept()
        with peer:
            peer.settimeout(5)
            assert read_frame(peer) == (HELLO, 1, struct.pack("!H", len(nick)) + nick)
            peer.sendall(frame(WELCOME, 1, struct.pack("!I", identifier)) + frame(ROSTER, 0, roster_payload([
                (identifier,"Алиса","127.0.0.1",1234),(identifier+1,"Боб","127.0.0.1",2345)])))
            # An unfinished input line must not prevent receiving an event.
            if incoming:
                process.stdin.write(b"/he"); process.stdin.flush()
                peer.sendall(frame(EVENT, 0, struct.pack("!II", identifier+1, 77) + "Ответ: привет!".encode()))
                seen = bytearray()
                while b"From " not in seen or "Ответ: привет!".encode() not in seen:
                    part = process.stdout.readline()
                    assert part, seen
                    seen.extend(part)
                commands = "lp\n" + commands
            process.stdin.write(commands.encode()); process.stdin.flush()
            for request, text in enumerate(texts, 2):
                assert read_frame(peer) == (CHAT, request, struct.pack("!I", identifier+1) + text.encode())
                peer.sendall(frame(ACCEPTED, request, struct.pack("!I", identifier+1)))
            assert read_frame(peer) == (GOODBYE, len(texts)+2, b"")
            assert peer.recv(1) == b""
            peer.shutdown(socket.SHUT_WR)
            output, error = process.communicate(timeout=5)
            assert process.returncode == 0, (output, error)
            if save_error:
                assert b"Cannot save history" in error, error
            return output.decode("utf-8")
    finally:
        if process.poll() is None: process.kill()
        process.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ui-smoke-", dir=Path.cwd()) as temporary, socket.socket() as listener:
        root = Path(temporary)
        listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(5)
        store = root / "журнал"
        output = session(args.client, listener, store,
            "/lang ru\n/help\n/msg 2 Привет, Боб!\n/lang en\n/msg 2 Русский при EN\n/quit\n",
            ("Привет, Боб!", "Русский при EN"), incoming=True)
        assert "Сменить язык интерфейса" in output and "Switch interface language" in output, output
        assert "Язык: Русский." in output and "Language: English." in output
        files = list(store.glob("*.wsh")); assert len(files) == 1
        records = journal(files[0]); assert len(records) == 3, records
        assert all(peer == "Боб" for _,peer,_ in records)
        session(args.client, listener, store, "/msg 42 После перезапуска\n/quit\n", ("После перезапуска",), identifier=41)
        records = journal(files[0]); assert len(records) == 4 and records[-1][2] == "После перезапуска", records
        before = files[0].read_bytes()
        session(args.client, listener, store, "/msg 2 Не сохранять\n/quit\n", ("Не сохранять",), options=("--no-history",))
        assert files[0].read_bytes() == before
        session(args.client, listener, root / "absent", "/quit\n", options=("--no-history",))
        assert not (root / "absent").exists()
        blocked = root / "blocked"; blocked.write_bytes(b"keep")
        session(args.client, listener, blocked, "/quit\n", save_error=True)
        assert blocked.read_bytes() == b"keep"
        session(args.client, listener, store, "/history off\n/msg 2 Отключено\n/quit\n", ("Отключено",))
        assert files[0].read_bytes() == before
        scope, nick = struct.unpack_from("!HH", before, 4)
        record = 12 + scope + nick
        for offset, replacement in ((8,struct.pack("!H",257)), (record+2,struct.pack("!H",33)),
                                    (record+4,struct.pack("!I",4097)), (record+1,b"\x01"),
                                    (record+16,b"\xff"), (12,b"X")):
            damaged = bytearray(before[:-4]); damaged[offset:offset+len(replacement)] = replacement
            damaged = bytes(damaged) + checksum(damaged)
            files[0].write_bytes(damaged)
            output = session(args.client, listener, store, "/quit\n")
            assert "damaged" in output and files[0].read_bytes() == damaged
        files[0].write_bytes(b"corrupt journal")
        output = session(args.client, listener, store, "/quit\n")
        assert "damaged" in output and files[0].read_bytes() == b"corrupt journal"
        session(args.client, listener, store, "/history clear\n/history off\n/quit\n")
        assert not files[0].exists()
        output = session(args.client, listener, store, "/help\n/lang en\n/help\n/quit\n", options=("--lang", "ru", "--no-history"))
        assert "Сменить язык интерфейса" in output and "Switch interface language" in output
    print("PASS: bilingual help, Cyrillic both locales, partial input, restart with changed IDs, history off/clear/corruption")


if __name__ == "__main__": main()
