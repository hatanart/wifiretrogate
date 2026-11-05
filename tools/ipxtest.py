#!/usr/bin/env python3
"""Send and receive IPX test frames in all four Ethernet encapsulations.

Each frame's payload names the encapsulation it was sent with, so the
listener can report "sent as X, arrived as Y" and reveal any conversion
done by Wi-Fi drivers, firmware or the access point.

Needs root (raw sockets). Standard library only.

  sudo python3 ipxtest.py listen <iface>
  sudo python3 ipxtest.py send <iface> [dest-mac] [count]
"""
import socket
import struct
import sys
import time

ETH_P_ALL = 0x0003
IPX_SOCKET = 0x7777          # not used by common games or NetWare
MAGIC = b"RGTEST"
ENCAPS = ["ethernet_ii", "802.2", "raw_802.3", "snap"]


def iface_mac(iface):
    with open(f"/sys/class/net/{iface}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def parse_mac(text):
    return bytes.fromhex(text.replace(":", "").replace("-", ""))


def fmt_mac(raw):
    return ":".join(f"{b:02x}" for b in raw)


def ipx_packet(src_mac, dst_mac, payload):
    length = 30 + len(payload)
    return struct.pack(
        ">HHBBI6sHI6sH",
        0xFFFF, length, 0, 4,             # checksum, length, hops, type (PEP)
        0, dst_mac, IPX_SOCKET,           # destination net/node/socket
        0, src_mac, IPX_SOCKET,           # source net/node/socket
    ) + payload


def build_frame(encap, dst, src, ipx):
    if encap == "ethernet_ii":
        body = struct.pack(">H", 0x8137) + ipx
    elif encap == "raw_802.3":
        body = struct.pack(">H", len(ipx)) + ipx
    elif encap == "802.2":
        llc = b"\xe0\xe0\x03"
        body = struct.pack(">H", len(llc) + len(ipx)) + llc + ipx
    elif encap == "snap":
        snap = b"\xaa\xaa\x03\x00\x00\x00\x81\x37"
        body = struct.pack(">H", len(snap) + len(ipx)) + snap + ipx
    frame = dst + src + body
    return frame + b"\x00" * max(0, 60 - len(frame))


def classify(frame):
    """Return (encapsulation, ipx bytes) or None if not IPX."""
    if len(frame) < 14 + 30:
        return None
    field = struct.unpack(">H", frame[12:14])[0]
    body = frame[14:]
    if field == 0x8137:
        return "ethernet_ii", body
    if field >= 0x0600:
        return None
    if body[:2] == b"\xff\xff":
        return "raw_802.3", body
    if body[:3] == b"\xe0\xe0\x03":
        return "802.2", body[3:]
    if body[:8] == b"\xaa\xaa\x03\x00\x00\x00\x81\x37":
        return "snap", body[8:]
    if body[:8] == b"\xaa\xaa\x03\x00\x00\xf8\x81\x37":
        # 802.1H bridge-tunnel header passed through unconverted
        return "bridge_tunnel", body[8:]
    return None


def send(iface, dst_text="ff:ff:ff:ff:ff:ff", count="3"):
    src = iface_mac(iface)
    dst = parse_mac(dst_text)
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((iface, 0))
    host = socket.gethostname().encode()
    for seq in range(int(count)):
        for encap in ENCAPS:
            payload = MAGIC + b" " + encap.encode() + b" " + str(seq).encode() + b" " + host
            s.send(build_frame(encap, dst, src, ipx_packet(src, dst, payload)))
            print(f"sent {encap:12} #{seq} {fmt_mac(src)} -> {fmt_mac(dst)}")
            time.sleep(0.2)
        time.sleep(1)


def listen(iface):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((iface, 0))
    print(f"listening for IPX test frames on {iface} (Ctrl+C to stop)")
    while True:
        frame, addr = s.recvfrom(2048)
        if addr[2] == socket.PACKET_OUTGOING:
            continue
        result = classify(frame)
        if result is None:
            continue
        arrived, ipx = result
        field = struct.unpack(">H", frame[12:14])[0]
        bad_length = field < 0x0600 and (field == 0 or field > len(frame) - 14)
        if bad_length:
            arrived += " BAD-LEN"   # DOS IPX stacks drop such frames
        idx = ipx.find(MAGIC)
        if idx < 0:
            print(f"other IPX  {arrived:12} from {fmt_mac(frame[6:12])}")
            continue
        fields = ipx[idx:].split(b"\x00")[0].decode(errors="replace").split()
        sent = fields[1] if len(fields) > 1 else "?"
        verdict = "BAD" if bad_length else "OK" if sent == arrived else "CONVERTED"
        print(f"{verdict:9} sent as {sent:12} arrived as {arrived:12} "
              f"from {fmt_mac(frame[6:12])} ({' '.join(fields[2:])})")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("send", "listen"):
        print(__doc__)
        sys.exit(1)
    if sys.argv[1] == "send":
        send(*sys.argv[2:])
    else:
        listen(sys.argv[2])
