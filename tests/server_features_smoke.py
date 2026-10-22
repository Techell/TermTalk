"""Independent WSCK v2 limits, file routing and real client transfer integration."""
import argparse
import ctypes
import hashlib
import msvcrt
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from wire import Peer, server, HELLO, ERROR, EVENT, CHAT, ACCEPTED, GOODBYE
from server_smoke import Application

STATUS, STATUS_REPLY = 0x22, 0x23
OFFER, OFFER_EVENT, CREATED, DECISION, DECISION_EVENT, DATA, ACK, END, DONE, CANCEL = range(0x40, 0x4a)
DEFAULT_LIMIT = 32 * 1024**3


def offer(peer, recipient, size, name=b"file.bin"):
    request = peer.send(OFFER, struct.pack("!IQH", recipient, size, len(name)) + name)
    return request


def rejected(peer, request, code=8):
    assert struct.unpack_from("!H", peer.until(ERROR, request)[2])[0] == code


def status(port, count, capacity, limit):
    probe = Peer(port)
    try:
        req = probe.send(STATUS)
        assert struct.unpack("!HHQ", probe.until(STATUS_REPLY, req)[2]) == (count, capacity, limit)
        probe.closed()
    finally:
        probe.close()


def routing(executable):
    with server(executable, "--max-clients", "3") as (port, _):
        a, b, stranger = Peer(port), Peer(port), Peer(port)
        try:
            for p, name in ((a, "Alice"), (b, "Боб"), (stranger, "Other")):
                p.hello(name)
            status(port, 3, 3, DEFAULT_LIMIT)  # Probe must work with all user slots occupied.
            extra = Peer(port)
            try:
                rejected(extra, extra.send(HELLO, b"\x00\x01X"), 6)
                extra.closed()
            finally:
                extra.close()
            rejected(a, offer(a, b.id, DEFAULT_LIMIT + 1))
            req = offer(a, b.id, DEFAULT_LIMIT)
            tid, recipient = struct.unpack("!II", a.until(CREATED, req)[2])
            assert recipient == b.id
            metadata = b.until(OFFER_EVENT, 0)[2]
            assert struct.unpack_from("!IIQ", metadata) == (tid, a.id, DEFAULT_LIMIT)
            b.send(DECISION, struct.pack("!IB", tid, 0))
            assert a.until(DECISION_EVENT, 0)[2] == struct.pack("!IB", tid, 0)
            req = offer(a, b.id, 3)
            tid, _ = struct.unpack("!II", a.until(CREATED, req)[2]); b.until(OFFER_EVENT, 0)
            rejected(a, a.send(DATA, struct.pack("!IQ", tid, 0) + b"abc"))  # Consent mandatory.
            rejected(stranger, stranger.send(CANCEL, struct.pack("!IB", tid, 1)))
            b.send(DECISION, struct.pack("!IB", tid, 1)); a.until(DECISION_EVENT, 0)
            rejected(a, a.send(DATA, struct.pack("!IQ", tid, 2**32 + 1) + b"a"))
            block = struct.pack("!IQ", tid, 0) + b"abc"
            a.send(DATA, block); assert b.until(DATA, 0)[2] == block
            rejected(a, a.send(DATA, block))  # One unacknowledged block only.
            rejected(b, b.send(ACK, struct.pack("!IQ", tid, 2)))
            b.send(ACK, struct.pack("!IQ", tid, 3)); a.until(ACK, 0)
            checksum = struct.pack("!I", tid) + hashlib.sha256(b"abc").digest()
            a.send(END, checksum); assert b.until(END, 0)[2] == checksum
            b.send(DONE, struct.pack("!I", tid)); a.until(DONE, 0)
            stranger.ping()
            assert not any(item[0] in range(OFFER_EVENT, CANCEL + 1) for item in stranger.inbox)
            assert set(a.roster()) == {a.id, b.id, stranger.id}
        finally:
            for p in (a, b, stranger): p.close()
    for limit in (0, 16):
        with server(executable, "--max-file-bytes", str(limit)) as (port, _):
            a, b = Peer(port), Peer(port)
            try:
                a.hello("A"); b.hello("B"); status(port, 2, 10, limit)
                rejected(a, offer(a, b.id, limit + 1))
                if limit:
                    req = offer(a, b.id, limit); a.until(CREATED, req); b.until(OFFER_EVENT, 0)
                else:
                    rejected(a, offer(a, b.id, 0))
            finally:
                a.close(); b.close()
    with server(executable, "--max-clients", "32", "--max-file-gib", "64") as (port, _):
        peers = [Peer(port) for _ in range(32)]
        try:
            for i, p in enumerate(peers): p.hello(f"User{i}")
            assert len(peers[0].roster()) == 32
            status(port, 32, 32, 64 * 1024**3)
        finally:
            for p in peers: p.close()
    for options in (("--max-clients", "0"), ("--max-clients", "33"), ("--max-clients", "9999999999999999999999"),
                    ("--max-file-gib", "1025"), ("--max-file-bytes", "-1"), ("--max-file-gib", "1", "--max-file-bytes", "1")):
        p = subprocess.run([str(executable), *options], capture_output=True, timeout=3)
        assert p.returncode == 1 and b"Usage:" in p.stderr, (options, p.stdout, p.stderr)


def real_files(executable, client):
    with tempfile.TemporaryDirectory(prefix="winsock-files-") as directory, server(executable) as (port, _):
        root = Path(directory)
        a, b = Application(client, port, "Алиса"), Application(client, port, "Bob")
        try:
            def start(content, name):
                source = root / name; source.write_bytes(content)
                before = len(b.lines); a.send(f'/sendfile {b.id} "{source}"')
                _, line = b.wait("File offer #", after=before)
                return int(re.search(r"File offer #(\d+)", line)[1]), source

            for index, content in enumerate((bytes(range(256)) * 513 + b"\x00\xffend", b"", b"abc")):
                tid, source = start(content, f"исходный файл {index}.bin")
                target = root / f"полученный файл {index}.bin"
                assert not target.exists() and not list(root.glob("*.part"))
                # Chat and ping remain available while the offer awaits consent.
                before = len(b.lines); a.send(f"/msg {b.id} Привет во время передачи")
                b.wait("Привет во время передачи", after=before)
                before = len(a.lines); a.send("/ping"); a.wait("PONG", after=before)
                b.send(f'/accept {tid} "{target}"')
                b.wait(f"File saved #{tid}", timeout=15); a.wait(f"File completed #{tid}", timeout=15)
                assert target.read_bytes() == content
                assert hashlib.sha256(target.read_bytes()).digest() == hashlib.sha256(source.read_bytes()).digest()
                assert not list(root.glob("*.part"))
            tid, _ = start(b"decline", "refuse.bin")
            b.send(f"/reject {tid}"); a.wait(f"File declined #{tid}")
            tid, _ = start(b"keep original", "exists.bin")
            target = root / "existing.bin"; target.write_bytes(b"original")
            before = len(b.lines); b.send(f'/accept {tid} "{target}"'); b.wait("Cannot use file", after=before)
            assert target.read_bytes() == b"original"; b.send(f"/reject {tid}"); a.wait(f"File declined #{tid}")
            tid, _ = start(bytes(range(256)) * 8192, "cancel.bin")
            target = root / "cancelled.bin"; b.send(f'/accept {tid} "{target}"')
            deadline = time.monotonic() + 3
            while not list(root.glob("*.part")) and time.monotonic() < deadline: time.sleep(.01)
            assert list(root.glob("*.part")); a.send("/cancel")
            a.wait(f"File cancelled #{tid}"); b.wait(f"File cancelled #{tid}")
            assert not target.exists() and not list(root.glob("*.part"))
            # Sparse metadata exercises GetFileSizeEx above 4 GiB without allocating/transferring 32 GiB.
            source = root / "over-limit-sparse.bin"
            with source.open("wb") as sparse:
                returned = ctypes.c_ulong()
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                                  ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
                assert kernel.DeviceIoControl(msvcrt.get_osfhandle(sparse.fileno()), 0x900c4, None, 0, None, 0,
                                              ctypes.byref(returned), None), ctypes.get_last_error()
                # CRT _chsize_s may explicitly zero-fill the gap. Native EOF on an
                # already sparse file reserves only logical size, without bulk writes.
                kernel.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_longlong, ctypes.c_void_p, ctypes.c_ulong]
                kernel.SetEndOfFile.argtypes = [ctypes.c_void_p]
                handle = msvcrt.get_osfhandle(sparse.fileno())
                assert kernel.SetFilePointerEx(handle, DEFAULT_LIMIT + 1, None, 0), ctypes.get_last_error()
                assert kernel.SetEndOfFile(handle), ctypes.get_last_error()
            before = len(a.lines); a.send(f'/sendfile {b.id} "{source}"')
            a.wait("file exceeds server limit", after=before); a.wait("File failed", after=before)
            assert source.stat().st_size == DEFAULT_LIMIT + 1
            tid, _ = start(bytes(range(256)) * 8192, "disconnect.bin")
            target = root / "interrupted.bin"; b.send(f'/accept {tid} "{target}"')
            deadline = time.monotonic() + 3
            while not list(root.glob("*.part")) and time.monotonic() < deadline: time.sleep(.01)
            assert list(root.glob("*.part")); b.send("/disconnect"); b.process.wait(timeout=5)
            a.wait(f"File failed #{tid}")
            assert not target.exists() and not list(root.glob("*.part"))
            # A committed transfer must not prevent a later normal transfer or quit.
            a.quit()
        finally:
            a.close(); b.close()
        # A sender with a valid frame but incorrect SHA must never commit the destination.
        b = Application(client, port, "ChecksumReceiver"); p = Peer(port)
        try:
            p.hello("ChecksumSender"); req = offer(p, b.id, 3)
            tid, _ = struct.unpack("!II", p.until(CREATED, req)[2]); b.wait(f"File offer #{tid}")
            target = root / "bad-hash.bin"; b.send(f'/accept {tid} "{target}"'); p.until(DECISION_EVENT, 0)
            p.send(DATA, struct.pack("!IQ", tid, 0) + b"abc"); p.until(ACK, 0)
            p.send(END, struct.pack("!I", tid) + bytes(32))
            assert p.until(CANCEL, 0)[2] == struct.pack("!IB", tid, 6)
            b.wait(f"File failed #{tid}")
            assert not target.exists() and not list(root.glob("*.part"))
            req = offer(p, b.id, 3); tid, _ = struct.unpack("!II", p.until(CREATED, req)[2]); b.wait(f"File offer #{tid}")
            target = root / "rename-race.bin"; b.send(f'/accept {tid} "{target}"'); p.until(DECISION_EVENT, 0)
            p.send(DATA, struct.pack("!IQ", tid, 0) + b"abc"); p.until(ACK, 0)
            target.write_bytes(b"created during transfer")
            p.send(END, struct.pack("!I", tid) + hashlib.sha256(b"abc").digest()); p.until(CANCEL, 0)
            b.wait(f"File failed #{tid}")
            assert target.read_bytes() == b"created during transfer" and not list(root.glob("*.part"))
            b.quit()
        finally:
            p.close(); b.close()


def switch_servers(executable, client):
    with tempfile.TemporaryDirectory(prefix="winsock-servers-") as directory:
        with server(executable) as (first, _), server(executable, "--max-clients", "2") as (second, _):
            app = Application(client, first, "Traveller", options=("--history-dir", directory), save_history=True)
            try:
                before = len(app.lines); app.send(f"/connect 127.0.0.1:{second}")
                app.wait("Registered as Traveller", after=before)
                before = len(app.lines); app.send("/status"); app.wait("1/2 users", after=before)
                before = len(app.lines); app.send("/servers"); app.wait(f"127.0.0.1:{first}", after=before)
                app.wait(f"127.0.0.1:{second}", after=before)
                app.send("/disconnect"); app.process.wait(timeout=5)
                assert app.process.returncode == 0, app.lines
            finally:
                app.close()
            text = (Path(directory) / "servers.wss").read_text()
            assert text.startswith("WSS1\n") and str(first) in text and str(second) in text
            status(first, 0, 10, DEFAULT_LIMIT); status(second, 0, 2, DEFAULT_LIMIT)
            # A different nickname loads the same address book after restarting.
            app = Application(client, first, "Reload", options=("--history-dir", directory), save_history=True)
            try:
                app.send("/servers"); app.wait(f"127.0.0.1:{second}"); app.quit()
            finally:
                app.close()


def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--server", type=Path, required=True); parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    routing(args.server); real_files(args.server, args.client); switch_servers(args.server, args.client)
    print("PASS: configurable 1..32 users, full-server probes, 64-bit file limits, consent/roles/offsets, real binary/Unicode/empty files, SHA-256, refusal/cancel, server switching and persistent address book")


if __name__ == "__main__": main()
