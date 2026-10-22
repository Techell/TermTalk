"""Independent Python implementation of the documented WSCK v2 wire format."""
import socket
import struct
import subprocess
from contextlib import contextmanager

HELLO, WELCOME, LIST, ROSTER = 1, 2, 3, 4
CHAT, EVENT, ACCEPTED, PING, PONG, GOODBYE, ERROR = 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x7f


def frame(kind, request, payload=b"", magic=b"WSCK", version=2, flags=0):
    return struct.pack("!4sBBHII", magic, version, kind, flags, len(payload), request) + payload


def exact(peer, length):
    data = bytearray()
    while len(data) < length:
        part = peer.recv(length - len(data))
        if not part:
            raise EOFError(("early EOF", len(data), length))
        data.extend(part)
    return bytes(data)


def read_frame(peer):
    magic, version, kind, flags, length, request = struct.unpack("!4sBBHII", exact(peer, 16))
    assert magic == b"WSCK" and version == 2 and flags == 0 and length <= 4108
    return kind, request, exact(peer, length)


def roster_payload(users):
    payload = struct.pack("!H", len(users))
    for identifier, nickname, ip, port in users:
        nickname = nickname.encode("utf-8")
        payload += struct.pack("!IBH", identifier, 1, len(nickname)) + nickname
        payload += socket.inet_aton(ip) + struct.pack("!H", port)
    return payload


def parse_roster(payload):
    count, = struct.unpack_from("!H", payload)
    at = 2
    users = {}
    for _ in range(count):
        identifier, status, length = struct.unpack_from("!IBH", payload, at)
        at += 7
        nickname = payload[at:at + length].decode("utf-8")
        at += length
        ip = socket.inet_ntoa(payload[at:at + 4])
        port, = struct.unpack_from("!H", payload, at + 4)
        at += 6
        assert identifier not in users and status == 1 and port != 0
        users[identifier] = (nickname, ip, port, status)
    assert at == len(payload) and count <= 32
    return users


class Peer:
    def __init__(self, port, slow=False):
        self.socket = socket.socket()
        self.socket.settimeout(5)
        if slow:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        self.socket.connect(("127.0.0.1", port))
        self.request = 0
        self.inbox = []
        self.id = None

    def close(self):
        self.socket.close()

    def send(self, kind, payload=b""):
        self.request += 1
        self.socket.sendall(frame(kind, self.request, payload))
        return self.request

    def until(self, kind, request=None):
        def match(item):
            return item[0] == kind and (request is None or item[1] == request)
        for index, item in enumerate(self.inbox):
            if match(item):
                return self.inbox.pop(index)
        while True:
            item = read_frame(self.socket)
            if match(item):
                return item
            self.inbox.append(item)

    def response(self, request):
        for index, item in enumerate(self.inbox):
            if item[1] == request and item[0] in (ACCEPTED, ERROR):
                return self.inbox.pop(index)
        while True:
            item = read_frame(self.socket)
            if item[1] == request and item[0] in (ACCEPTED, ERROR):
                return item
            self.inbox.append(item)

    def hello(self, nickname):
        nickname = nickname.encode("utf-8") if isinstance(nickname, str) else nickname
        request = self.send(HELLO, struct.pack("!H", len(nickname)) + nickname)
        reply = self.until(WELCOME, request)
        self.id, = struct.unpack("!I", reply[2])
        assert self.id != 0
        return self.id

    def roster(self):
        request = self.send(LIST)
        return parse_roster(self.until(ROSTER, request)[2])

    def ping(self):
        request = self.send(PING)
        assert self.until(PONG, request)[2] == b""

    def closed(self, timeout=5):
        self.socket.settimeout(timeout)
        while True:
            try:
                self.inbox.append(read_frame(self.socket))
            except (EOFError, ConnectionResetError):
                return


@contextmanager
def server(executable, *options):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([str(executable), str(port), *options], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert process.stdout.readline().rstrip(b"\r\n") == b"+---+  TermTalk"
        assert process.stdout.readline().rstrip(b"\r\n") == b"| > |  Messages. Files. Terminal."
        assert process.stdout.readline().rstrip(b"\r\n") == b"+---+"
        line = process.stdout.readline()
        assert line.startswith(b"Listening on port"), (line, process.communicate(timeout=5))
        yield port, process
        assert process.poll() is None, process.communicate(timeout=5)
    finally:
        if process.poll() is None:
            process.terminate()
        process.communicate(timeout=5)
