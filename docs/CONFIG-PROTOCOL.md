# Raw-frame configuration protocol

Lets a tool on the retro machine run console commands **without any TCP/IP**:
no IP address, gateway or DHCP, only a link and a driver that can send raw
Ethernet frames (Linux raw sockets, a DOS packet driver, an Amiga SANA-II
device).

- EtherType **0x88B5** (IEEE 802 local experimental).
- Requests go to the broadcast address `ff:ff:ff:ff:ff:ff`; the tool needs no
  address of the WifiRetroGate.
- WifiRetroGate handles every 0x88B5 frame on its Ethernet side and never
  forwards it to Wi-Fi. 0x88B5 frames arriving from Wi-Fi are ignored.
- Replies go to the requesting MAC, from the ESP32's own Ethernet MAC.

## Frame layout

```text
offset  size  field
0       6     destination MAC (request: ff:ff:ff:ff:ff:ff)
6       6     source MAC
12      2     EtherType 0x88B5
14      3     magic "WRG"
17      1     version: 1
18      1     type: 1 = request, 2 = reply
19      2     sequence number, big endian (copied into the reply)
21      1     part number (reply: 0, 1, 2, ...; request: 0)
22      1     flags: bit 0 = last part (request: 1)
23      ...   text, UTF-8: the command line (request) or output (reply)
```

Frames shorter than 60 bytes are padded with zeros; the request text ends at
the first zero byte. A reply longer than 1400 bytes of text is split over
several frames; the tool collects parts until the one with the "last" flag.
If no reply arrives, the tool repeats the request with the same sequence number
(commands may then run twice; all commands are harmless to repeat).

The commands and their output are those of the serial/Telnet console
(`help` lists them).

## Implementations

- Firmware: `firmware/main/rawcfg.c`
- Linux: `tools/wrg.py`
- DOS (`WRG.COM`, packet driver interface): planned
