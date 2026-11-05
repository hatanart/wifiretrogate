#!/usr/bin/env python3
"""WifiRetroGate configuration tool (raw Ethernet frames, no TCP/IP needed).

Sends a console command to the WifiRetroGate on the given Ethernet interface
and prints the reply. Without a command, starts an interactive prompt.

  sudo python3 wrg.py eth0 status
  sudo python3 wrg.py eth0 wifi add "My Network" password123
  sudo python3 wrg.py eth0

Needs root (raw sockets). Standard library only.
"""
import os
import random
import select
import socket
import struct
import sys

ETHERTYPE = 0x88B5
MAGIC = b"WRG"
VERSION = 1
TYPE_REQUEST = 1
TYPE_REPLY = 2
FLAG_LAST = 0x01
HEADER = struct.Struct(">3sBBHBB")	# magic, version, type, seq, part, flags
BROADCAST = b"\xff" * 6

# scanning takes a few seconds on the device
REPLY_TIMEOUT = 8.0
RETRIES = 3


def iface_mac(iface):
    with open(f"/sys/class/net/{iface}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def build_request(src_mac, seq, command):
    text = command.encode("utf-8")
    frame = BROADCAST + src_mac + struct.pack(">H", ETHERTYPE)
    frame += HEADER.pack(MAGIC, VERSION, TYPE_REQUEST, seq, 0, FLAG_LAST) + text
    return frame + b"\x00" * max(0, 60 - len(frame))


def parse_reply(frame, my_mac, seq):
    """Return (part, last, text) for a reply to us with this sequence number."""
    if len(frame) < 14 + HEADER.size or frame[:6] != my_mac:
        return None
    if struct.unpack(">H", frame[12:14])[0] != ETHERTYPE:
        return None
    magic, version, kind, rseq, part, flags = HEADER.unpack_from(frame, 14)
    if magic != MAGIC or version != VERSION or kind != TYPE_REPLY or rseq != seq:
        return None
    text = frame[14 + HEADER.size:].rstrip(b"\x00")
    return part, bool(flags & FLAG_LAST), text.decode("utf-8", "replace")


def run_command(sock, iface, command):
    my_mac = iface_mac(iface)
    seq = random.randrange(0x10000)
    request = build_request(my_mac, seq, command)

    for attempt in range(RETRIES):
        sock.send(request)
        parts = {}
        while True:
            ready, _, _ = select.select([sock], [], [], REPLY_TIMEOUT)
            if not ready:
                break
            reply = parse_reply(sock.recv(2048), my_mac, seq)
            if reply is None:
                continue
            part, last, text = reply
            parts[part] = text
            if last and len(parts) == part + 1:
                return "".join(parts[i] for i in range(part + 1))
    return None


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 1
    iface = sys.argv[1]
    if os.geteuid() != 0:
        print("needs root: sudo python3 wrg.py ...", file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE))
    sock.bind((iface, 0))

    def do(command):
        output = run_command(sock, iface, command)
        if output is None:
            print(f"no answer from WifiRetroGate on {iface}", file=sys.stderr)
            return False
        print(output, end="" if output.endswith("\n") else "\n")
        return True

    if len(sys.argv) > 2:
        # keep quoting for names with spaces: wifi add "My Network" pw
        words = [f'"{w}"' if (" " in w or not w) else w for w in sys.argv[2:]]
        return 0 if do(" ".join(words)) else 1

    print("WifiRetroGate console over raw Ethernet, Ctrl+D to quit")
    while True:
        try:
            line = input("wrg> ")
        except EOFError:
            print()
            return 0
        if line.strip():
            do(line)


if __name__ == "__main__":
    sys.exit(main())
