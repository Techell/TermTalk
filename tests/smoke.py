"""Validate the C client against an independent Python WSCK endpoint."""
import argparse
import socket
import struct
import subprocess
from pathlib import Path
from wire import *


def check_client(executable, mode):
    nickname="Алиса" if mode=="unicode" else "Alice"
    nick=nickname.encode("utf-8")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(5)
        process = subprocess.Popen([str(executable), "127.0.0.1", str(listener.getsockname()[1]), nickname, "--no-history"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(5)
                assert read_frame(peer) == (HELLO, 1, struct.pack("!H",len(nick))+nick)
                if mode == "no-welcome":
                    peer.shutdown(socket.SHUT_WR)
                    out, err = process.communicate(timeout=5)
                    assert process.returncode == 1, (out, err)
                    return
                welcome = frame(WELCOME, 1, struct.pack("!I", 1))
                roster = frame(ROSTER, 0, roster_payload([(1,nickname,"127.0.0.1",1234),(2,"Bob","127.0.0.1",2345)]))
                # Both fragmented headers and a coalesced payload/next frame.
                peer.sendall(welcome[:3]); peer.sendall(welcome[3:17]); peer.sendall(welcome[17:] + roster)
                if mode in ("exchange","unicode"):
                    process.stdin.write(b"/to 2\nhello\n/ping\n"); process.stdin.flush()
                    assert read_frame(peer) == (CHAT, 2, struct.pack("!I", 2) + b"hello")
                    assert read_frame(peer) == (PING, 3, b"")
                    peer.sendall(frame(EVENT, 0, struct.pack("!II", 2, 77) + b"reply")
                                 + frame(ACCEPTED, 2, struct.pack("!I", 2)) + frame(PONG, 3))
                    peer.shutdown(socket.SHUT_WR)
                    out, err = process.communicate(timeout=5)
                    assert process.returncode == 0 and b"From 2 (Bob): reply" in out, (out, err)
                    assert b"delivery is not confirmed" in out and b"PONG" in out, out
                elif mode == "eof":
                    process.stdin.close(); process.stdin = None
                    assert peer.recv(1) == b""
                    peer.sendall(frame(EVENT, 0, struct.pack("!II",2,99) + b"reply-after-eof"))
                    peer.shutdown(socket.SHUT_WR)
                    out, err = process.communicate(timeout=5)
                    assert process.returncode == 0 and b"reply-after-eof" in out, (out, err)
                elif mode == "quit":
                    process.stdin.write(b"/quit\n"); process.stdin.flush()
                    assert read_frame(peer) == (GOODBYE, 2, b"")
                    assert peer.recv(1) == b""
                    peer.shutdown(socket.SHUT_WR)
                    out, err = process.communicate(timeout=5)
                    assert process.returncode == 0, (out, err)
                elif mode == "oversized":
                    out, err = process.communicate(input=b"x"*4160+b"\n", timeout=5)
                    assert process.returncode == 1 and peer.recv(4096) == b"", (out, err)
                else:
                    event = frame(EVENT, 0, struct.pack("!II",2,99)+b"broken")
                    peer.sendall(event[:17] if mode == "truncated" else b"BAD!"+event[4:])
                    peer.shutdown(socket.SHUT_WR)
                    out, err = process.communicate(timeout=5)
                    assert process.returncode == 1, (out, err)
        finally:
            if process.poll() is None: process.kill()
            process.communicate(timeout=5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server",type=Path,required=True)
    parser.add_argument("--client",type=Path,required=True)
    args=parser.parse_args()
    for executable in (args.server,args.client):
        for port in ("0","65536","-1","12abc","99999999999999999999"):
            argv=[str(executable),port] if executable==args.server else [str(executable),"127.0.0.1",port]
            result=subprocess.run(argv,capture_output=True,timeout=5)
            assert result.returncode==1 and b"Usage:" in result.stderr,result
    for nickname in ("", "a"*33, "bad\nname"):
        result=subprocess.run([str(args.client),"127.0.0.1","5050",nickname],capture_output=True,timeout=5)
        assert result.returncode==1 and b"Usage:" in result.stderr,result
    result=subprocess.run([str(args.client),"invalid-ip"],capture_output=True,timeout=5)
    assert result.returncode==1 and b"Connect failed:" in result.stderr,result
    with socket.socket() as occupied:
        occupied.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
        occupied.bind(("127.0.0.1",0)); occupied.listen(1)
        result=subprocess.run([str(args.server),str(occupied.getsockname()[1])],capture_output=True,timeout=5)
        assert result.returncode==1 and b"initialization failed:" in result.stderr,result
    for mode in ("exchange","unicode","eof","quit","oversized","truncated","malformed","no-welcome"):
        check_client(args.client,mode)
    print("PASS: client handshake, roster, commands, frames, EOF, malformed server, arguments and busy port")


if __name__=="__main__": main()
