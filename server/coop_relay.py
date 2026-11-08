"""Co-op relay: every game connects here over UDP and the relay forwards each player's state to the others.

    python server/coop_relay.py [--port 41900]

Packets (little-endian), all starting with the header  b'TKCO', version u8, type u8, player id u8, reserved u8:
  1 HELLO    client -> relay: name (16 bytes, NUL padded). Answered with WELCOME.
  2 WELCOME  relay -> client: the client's player id in the header, then the ids and names of everyone else.
  3 STATE    client -> relay -> everyone else: the sender's state (runtime/coop_net.c NetState); the relay only
             writes the sender's id into the header.
  4 LEAVE    relay -> everyone: the player in the header left (said so, or silent for TIMEOUT seconds).
  5 BYE      client -> relay: leaving now.
Any player may join or leave at any time; ids 1..MAX_PLAYERS are handed out on HELLO and freed on leave.
"""
import argparse
import socket
import struct
import sys
import time

MAGIC, VERSION = b'TKCO', 23
HELLO, WELCOME, STATE, LEAVE, BYE, WORLD, HIT, POSE, OBJS, EPOSE = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
MAX_PLAYERS, TIMEOUT = 8, 5.0
HEADER = struct.Struct('<4sBBBB')


def header(kind, pid):
    return HEADER.pack(MAGIC, VERSION, kind, pid, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--port', type=int, default=41900)
    ap.add_argument('--log', help='write the lines here instead of to the console (for pythonw, which has none)')
    a = ap.parse_args()
    if a.log:
        sys.stdout = sys.stderr = open(a.log, 'w', buffering=1, encoding='utf-8')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)   # enemy poses come in bursts
    sock.bind(('0.0.0.0', a.port))
    sock.settimeout(0.5)
    players = {}   # addr -> {'id', 'name', 'seen'}
    print(f'relay: listening on UDP {a.port}', flush=True)

    def send(data, addr):
        # A send can fail (the network gone for a moment, a peer unreachable): only recvfrom was guarded, and a
        # failed send ended the relay for everyone (code review, 2026-11-06).
        try:
            sock.sendto(data, addr)
        except OSError as e:
            print(f'relay: could not send to {addr[0]}:{addr[1]}: {e}', flush=True)

    def leave(addr, why):
        p = players.pop(addr)
        print(f'relay: player {p["id"]} "{p["name"]}" left ({why}); {len(players)} connected', flush=True)
        for other in players:
            send(header(LEAVE, p['id']), other)

    while True:
        try:
            data, addr = sock.recvfrom(8192)
        except socket.timeout:
            data, addr = None, None
        except ConnectionResetError:   # Windows reports an unreachable earlier peer this way
            continue
        now = time.monotonic()
        for other in [k for k, p in players.items() if now - p['seen'] > TIMEOUT]:
            leave(other, 'timed out')
        if not data or len(data) < HEADER.size:
            continue
        magic, version, kind, _, _ = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION:
            continue
        if kind == HELLO:
            name = data[HEADER.size:HEADER.size + 16].split(b'\0')[0].decode('utf-8', 'replace')
            if addr not in players:
                used = {p['id'] for p in players.values()}
                free = [i for i in range(1, MAX_PLAYERS + 1) if i not in used]
                if not free:
                    print(f'relay: {addr} refused: {MAX_PLAYERS} players already', flush=True)
                    continue
                players[addr] = {'id': free[0], 'name': name, 'seen': now}
                print(f'relay: player {free[0]} "{name}" joined from {addr[0]}:{addr[1]}; {len(players)} connected', flush=True)
            me = players[addr]
            me['seen'] = now
            others = b''.join(struct.pack('<B16s', p['id'], p['name'].encode()[:16]) for k, p in players.items() if k != addr)
            send(header(WELCOME, me['id']) + others, addr)
        elif kind in (STATE, WORLD, HIT, POSE, OBJS, EPOSE) and addr in players:
            me = players[addr]
            me['seen'] = now
            out = header(kind, me['id']) + data[HEADER.size:]
            for other in players:
                if other != addr:
                    send(out, other)
        elif kind == BYE and addr in players:
            leave(addr, 'said bye')


if __name__ == '__main__':
    main()
