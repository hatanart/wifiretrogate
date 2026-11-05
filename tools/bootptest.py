#!/usr/bin/env python3
"""Does the local network answer plain BOOTP (as Trumpet Winsock 2.x sends it)?

Sends a BOOTP request without any DHCP options and prints any reply. Uses the
interface's own MAC as the client hardware address; the reply is requested as
a broadcast, so the current IP configuration of this machine does not matter.

  sudo python3 bootptest.py wlp0s20f3

Needs root (UDP port 68). Standard library only. Does not change any settings;
a server that answers may reserve an address for this MAC for a while.
"""
import os
import random
import select
import socket
import struct
import sys
import time

MAGIC_COOKIE = b"\x63\x82\x53\x63"


def iface_mac(iface):
    with open(f"/sys/class/net/{iface}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def bootp_request(mac, xid):
    return struct.pack(
        "!BBBBIHH4s4s4s4s16s64s128s",
        1, 1, 6, 0,             # BOOTREQUEST, Ethernet, hlen 6, hops
        xid, 0, 0x8000,         # transaction id, seconds, flags: broadcast reply
        b"\0" * 4, b"\0" * 4, b"\0" * 4, b"\0" * 4,   # ciaddr yiaddr siaddr giaddr
        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128,
    ) + MAGIC_COOKIE + b"\xff" + b"\0" * 59   # vendor area: cookie, end (no option 53)


def parse_options(data):
    options, i = {}, 0
    while i < len(data) and data[i] != 255:
        if data[i] == 0:
            i += 1
            continue
        code, length = data[i], data[i + 1]
        options[code] = data[i + 2:i + 2 + length]
        i += 2 + length
    return options


def ip(b):
    return ".".join(str(x) for x in b)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    if os.geteuid() != 0:
        print("needs root: sudo python3 bootptest.py IFACE", file=sys.stderr)
        return 1
    iface = sys.argv[1]
    mac = iface_mac(iface)
    xid = random.getrandbits(32)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())
    sock.bind(("", 68))

    print(f"BOOTP request on {iface} for {mac.hex(':')} ...")
    deadline = time.time() + 10
    sent = 0
    while time.time() < deadline:
        if sent < 3:
            sock.sendto(bootp_request(mac, xid), ("255.255.255.255", 67))
            sent += 1
        ready, _, _ = select.select([sock], [], [], 3)
        if not ready:
            continue
        data, addr = sock.recvfrom(2048)
        if len(data) < 240 or data[0] != 2 or struct.unpack("!I", data[4:8])[0] != xid:
            continue
        options = parse_options(data[240:]) if data[236:240] == MAGIC_COOKIE else {}
        if 53 in options:
            print(f"Reply from {addr[0]}, but it is a DHCP message (type {options[53][0]}):")
            print("the server treated the request as DHCP. Trumpet expects plain BOOTP.")
        else:
            print(f"BOOTP reply from {addr[0]}: the network answers BOOTP")
        print(f"  address  {ip(data[16:20])}")
        if 1 in options:
            print(f"  netmask  {ip(options[1])}")
        if 3 in options:
            print(f"  gateway  {ip(options[3][:4])}")
        if 6 in options:
            print(f"  DNS      {', '.join(ip(options[6][i:i + 4]) for i in range(0, len(options[6]), 4))}")
        return 0

    print("No answer: the network does not answer plain BOOTP.")
    print("Trumpet Winsock 2.x would need a static IP, or WifiRetroGate's BOOTP-to-DHCP translation.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
