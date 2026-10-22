"""Independent WSCK clients and actual C applications exercise the private-chat server."""
import argparse
import re
import struct
import subprocess
import threading
import time
from pathlib import Path
from wire import *


def error(peer, request, code):
    reply=peer.until(ERROR,request)
    assert struct.unpack_from("!H",reply[2])[0]==code,reply
    assert struct.unpack_from("!H",reply[2],2)[0]==len(reply[2])-4
    return reply


def event(peer, sender, request, text):
    kind, identifier, payload=peer.until(EVENT)
    assert identifier==0 and payload==struct.pack("!II",sender,request)+text,(kind,identifier,payload[:80])


def accepted(peer, request, target):
    assert peer.until(ACCEPTED,request)[2]==struct.pack("!I",target)


def check_registration_and_privacy(executable):
    with server(executable) as (port,_):
        peers=[]
        try:
            for i in range(10):
                peer=Peer(port); peers.append(peer)
                if i==0:
                    peer.request=1
                    encoded=frame(HELLO,1,b"\x00\x05Alice")
                    for byte in encoded: peer.socket.sendall(bytes([byte]))
                    peer.id,=struct.unpack("!I",peer.until(WELCOME,1)[2])
                else: peer.hello(f"User{i}")
            ids=[p.id for p in peers]
            assert len(set(ids))==10
            users=peers[0].roster()
            assert len(users)==10 and set(users)==set(ids)
            for p in peers:
                assert users[p.id][1:]==("127.0.0.1",p.socket.getsockname()[1],1)
            # Automatic updates are full snapshots, without a LIST_REQUEST.
            while len(parse_roster(peers[0].until(ROSTER,0)[2])) != 10: pass
            excess=Peer(port)
            try:
                req=excess.send(HELLO,b"\x00\x06Excess")
                error(excess,req,6)
                excess.closed()
            finally: excess.close()
            for i,source in enumerate(peers):
                target=peers[(i+1)%10]
                text=f"private-{i}".encode()
                req=source.send(CHAT,struct.pack("!I",target.id)+text)
                accepted(source,req,target.id); event(target,source.id,req,text)
            # Coalesced commands plus a deliberately split header and a maximum text.
            first,target,outsider=peers[:3]
            messages=["Привет 🌍".encode(),b"x"*4096,b"last"]
            base=first.request
            joined=b"".join(frame(CHAT,base+i+1,struct.pack("!I",target.id)+text) for i,text in enumerate(messages))
            first.request+=len(messages)
            first.socket.sendall(joined[:7]); first.socket.sendall(joined[7:])
            for i,text in enumerate(messages):
                accepted(first,base+i+1,target.id); event(target,first.id,base+i+1,text)
            outsider.ping()
            assert not any(item[0]==EVENT for item in outsider.inbox),outsider.inbox
            req=first.send(CHAT,struct.pack("!I",first.id)+b"self")
            error(first,req,4)
            req=first.send(CHAT,struct.pack("!I",999999)+b"missing")
            error(first,req,4)
            first.ping()  # Ordinary errors do not disconnect the sender.
            old=peers[-1]; old_id=old.id
            old.send(GOODBYE); old.closed(); old.close()
            while len(parse_roster(first.until(ROSTER,0)[2])) != 9: pass
            replacement=Peer(port); peers[-1]=replacement; replacement.hello("User9")
            assert replacement.id>max(ids) and replacement.id!=old_id
            assert len(first.roster())==10
            req=first.send(CHAT,struct.pack("!I",old_id)+b"stale")
            error(first,req,4)
            req=first.send(CHAT,struct.pack("!I",replacement.id)+b"new session")
            accepted(first,req,replacement.id); event(replacement,first.id,req,b"new session")
        finally:
            for peer in peers: peer.close()


def check_errors(executable):
    with server(executable) as (port,_):
        owner=Peer(port); owner.hello("Alice")
        try:
            duplicate=Peer(port)
            try:
                req=duplicate.send(HELLO,b"\x00\x05Alice"); error(duplicate,req,3)
                duplicate.hello("Bob")  # Retry registration with a new request ID.
                req=duplicate.send(HELLO,b"\x00\x05Other"); error(duplicate,req,1)
                duplicate.ping()
            finally: duplicate.close()
            # Unknown directions/state and replayed/zero IDs produce typed errors.
            for mode in ("state","direction","duplicate","zero","decreasing"):
                peer=Peer(port)
                try:
                    if mode=="state": req=peer.send(LIST); error(peer,req,1)
                    else:
                        peer.hello("Negative")
                        if mode=="direction": req=peer.send(WELCOME,struct.pack("!I",1)); error(peer,req,2)
                        elif mode=="decreasing":
                            peer.socket.sendall(frame(PING,10)); peer.until(PONG,10)
                            peer.socket.sendall(frame(LIST,9)); error(peer,9,7)
                        else:
                            req=1 if mode=="duplicate" else 0
                            peer.socket.sendall(frame(LIST,req)); error(peer,req,7)
                    peer.closed()
                finally: peer.close()
            bad_frames=[
                frame(HELLO,1,b"\x00\x01x",magic=b"BAD!"),
                frame(HELLO,1,b"\x00\x01x",version=1),
                frame(HELLO,1,b"\x00\x01x",flags=1),
                frame(0x99,1),
                struct.pack("!4sBBHII",b"WSCK",2,HELLO,0,0xffffffff,1),
                frame(HELLO,1,b"\x00\x00"),
                frame(HELLO,1,b"\x00\x21"+b"x"*33),
                frame(HELLO,1,b"\x00\x02\xc0\xaf"),
                frame(HELLO,1,b"\x00\x01\x00"),
                frame(HELLO,1,b"\x00\x03a\x1bb"),
                frame(HELLO,1,b"\x00\x03\xed\xa0\x80"),
                frame(HELLO,1,b"\x00\x02\xc2\x9b"),
            ]
            for encoded in bad_frames:
                peer=Peer(port)
                try: peer.socket.sendall(encoded); peer.closed()
                finally: peer.close()
            for cut in (7,17):
                peer=Peer(port)
                try:
                    encoded=frame(HELLO,1,b"\x00\x05CutMe")
                    peer.socket.sendall(encoded[:cut]); peer.socket.shutdown(socket.SHUT_WR); peer.closed()
                finally: peer.close()
            for invalid in (b"bad\x00text",b"bad\x1b[31m",b"\xed\xa0\x80",b"",b"x"*4097):
                peer=Peer(port)
                try:
                    peer.hello("BadText")
                    peer.send(CHAT,struct.pack("!I",owner.id)+invalid); peer.closed()
                finally: peer.close()
            owner.ping()
            assert set(owner.roster())=={owner.id}
            assert not any(item[0]==EVENT for item in owner.inbox)
        finally: owner.close()


def check_queue(executable):
    with server(executable) as (port,_):
        source=Peer(port); target=Peer(port,slow=True); fast=Peer(port)
        try:
            source.hello("Source"); target.hello("Slow"); fast.hello("Fast")
            expected=[]; refused=None; text=b"q"*4096
            for _ in range(2048):
                req=source.send(CHAT,struct.pack("!I",target.id)+text)
                reply=source.response(req)
                if reply[0]==ERROR:
                    assert struct.unpack_from("!H",reply[2])[0]==5,reply
                    refused=req; break
                assert reply[2]==struct.pack("!I",target.id)
                expected.append(req)
            assert refused is not None,"Could not fill the bounded recipient queue"
            req=source.send(CHAT,struct.pack("!I",fast.id)+b"fast survives")
            accepted(source,req,fast.id); event(fast,source.id,req,b"fast survives")
            for request in expected: event(target,source.id,request,text)
            target.ping()
            assert not any(item[0]==EVENT and struct.unpack_from("!I",item[2],4)[0]==refused for item in target.inbox)
            req=source.send(CHAT,struct.pack("!I",target.id)+b"after drain")
            accepted(source,req,target.id); event(target,source.id,req,b"after drain")
        finally:
            source.close(); target.close(); fast.close()


def check_timeouts(executable):
    with server(executable) as (port,_):
        silent=Peer(port); partial=Peer(port); healthy=Peer(port)
        try:
            partial.hello("Partial"); healthy.hello("Healthy")
            incomplete=frame(PING,2)
            partial.socket.sendall(incomplete[:6])
            # Dripping bytes must not extend the absolute ten-second frame deadline.
            for i in range(4):
                time.sleep(2)
                partial.socket.sendall(incomplete[6+i:7+i])
                healthy.ping()
            silent.closed(timeout=4)
            partial.closed(timeout=4)
            healthy.ping()
            assert set(healthy.roster())=={healthy.id}
        finally:
            silent.close(); partial.close(); healthy.close()


class Application:
    def __init__(self,executable,port,nickname,options=(),save_history=False):
        self.process=subprocess.Popen([str(executable),"127.0.0.1",str(port),nickname,*([] if save_history else ["--no-history"]),*options],stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.lines=[]; self.condition=threading.Condition()
        def capture():
            for line in iter(self.process.stdout.readline,b""):
                with self.condition:
                    self.lines.append(line.decode("utf-8",errors="replace").rstrip("\r\n"))
                    self.condition.notify_all()
        self.reader=threading.Thread(target=capture,daemon=True); self.reader.start()
        _,line=self.wait("Registered as ")
        self.id=int(re.search(r"id=(\d+)",line).group(1))

    def wait(self,text,after=0,timeout=5):
        deadline=time.monotonic()+timeout
        with self.condition:
            while True:
                for index,line in enumerate(self.lines[after:],after):
                    if text in line: return index,line
                remaining=deadline-time.monotonic()
                assert remaining>0,(text,self.lines[-15:])
                self.condition.wait(remaining)

    def send(self,text):
        self.process.stdin.write(text.encode("utf-8")+b"\n"); self.process.stdin.flush()

    def quit(self):
        self.send("/quit"); self.process.wait(timeout=5)
        assert self.process.returncode==0,self.process.stderr.read()

    def close(self):
        if self.process.poll() is None: self.process.kill()
        self.process.wait(timeout=5); self.reader.join(timeout=5)
        self.process.stdin.close(); self.process.stdout.close(); self.process.stderr.close()


def check_applications(executable,client):
    with server(executable) as (port,_):
        applications=[]
        try:
            for name in ("Alice","Bob","Чарли"):
                applications.append(Application(client,port,name))
            alice,bob,charlie=applications
            alice.wait(" | Bob | "); bob.wait(" | Alice | ")
            alice.wait("Participants (3):"); bob.wait("Participants (3):"); charlie.wait("Participants (3):")
            alice.send(f"/to {bob.id}"); alice.wait(f"Selected recipient {bob.id}.")
            alice.send("first-to-second")
            bob.wait(f"From {alice.id} (Alice): first-to-second")
            alice.wait("delivery is not confirmed")
            bob.send(f"/msg {alice.id} second-to-first")
            alice.wait(f"From {bob.id} (Bob): second-to-first")
            bob.wait("delivery is not confirmed")
            charlie.send("/ping"); charlie.wait("PONG")
            assert not any(line.startswith("From ") for line in charlie.lines)
            before=len(alice.lines); bob.quit()
            alice.wait("Participants (2):",after=before)
            alice.send("recipient-left"); alice.wait("Select a recipient with /to")
            alice.send(f"/msg {bob.id} unavailable"); alice.wait("Server error 4")
            alice.send("/msg 999999999999999999999 overflow"); alice.wait("Usage: /msg ID text")
            alice.quit(); charlie.quit()
        finally:
            for application in applications: application.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server",type=Path,required=True); parser.add_argument("--client",type=Path,required=True)
    args=parser.parse_args()
    check_registration_and_privacy(args.server)
    check_errors(args.server)
    check_queue(args.server)
    check_timeouts(args.server)
    check_applications(args.server,args.client)
    print("PASS: registration, live roster, ten clients, private routing, invalid frames/IDs, bounded queues, deadlines and real C clients")


if __name__=="__main__": main()
