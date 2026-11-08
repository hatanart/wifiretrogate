# How WifiRetroGate works

## The problem

An 802.11 client ("station") sends and receives frames with its own MAC
address only. The access point delivers to the station's MAC and does not know
about other machines behind it. So a station cannot simply bridge an Ethernet
segment with several MAC addresses.

The usual workarounds:

- **MAC translation**: rewrite the wired machine's MAC to the station's MAC and
  back. Protocols that carry MAC addresses inside the payload must be patched
  too (ARP, DHCP, ...). IPX uses the MAC as its node address, so IPX breaks.
  Espressif's `sta2eth` example works this way.
- **Routing / NAT / proxy ARP**: IP only, no IPX, NetBEUI or AppleTalk.
- **4-address (WDS) mode**: needs support and configuration on the access point.

## The trick: one client, cloned MAC

WifiRetroGate supports exactly one wired machine and gives the Wi-Fi station
**that machine's MAC address**. Frames are forwarded unchanged:

```text
 retro machine           WifiRetroGate                      access point
 MAC 00:40:63:12:34:56   Ethernet: promiscuous              sees a Wi-Fi client
 -------------------->   Wi-Fi station MAC = 00:40:63:12:34:56  with that MAC
                         frames from the client  --> Wi-Fi
                         frames from Wi-Fi       --> Ethernet
```

The access point sees the retro machine as an ordinary Wi-Fi client. DHCP gives
it a normal address under its own MAC, ARP and IPX node addresses are correct,
and nothing in the payload needs touching. A Wi-Fi station only receives frames
for its own MAC plus broadcasts and multicasts, which is exactly what a single
machine on a cable would receive.

## Learning the client

- The MAC of the first valid frame on Ethernet becomes the client: unicast
  source address, not a link-local switch protocol (`01:80:C2:00:00:0x`: STP,
  LLDP, LACP). It is saved in flash.
- With a saved MAC, Wi-Fi connects at power-on, before the retro machine starts
  networking. Without one, Wi-Fi starts after the first frame (that frame is
  lost; protocols retry).
- A **different** machine is accepted only after the Ethernet link went down
  and up (or after a restart), never while the link stays up, so a busy switch
  cannot make it flip between machines. Frames from other machines are dropped
  and listed by the `macs` command.
- Changing the client: save the new MAC, leave the Wi-Fi network with a
  deauthentication frame (so the access point drops the old association at
  once), restart, connect with the new MAC.

## Forwarding rules

Ethernet -> Wi-Fi: only frames from the client, except
- configuration frames (EtherType 0x88B5), handled locally;
- IPv4 packets for the **service address** (default `192.0.2.1`), handled by
  the local Telnet console.

Wi-Fi -> Ethernet: everything the station receives, except
- frames with the client's own MAC as source (the access point repeating the
  client's broadcasts);
- configuration frames (never accepted from the Wi-Fi side).

Wi-Fi power saving is **off**: a sleeping station misses broadcasts, and IPX
game discovery depends on them.

## ESP32 quirks and how the firmware handles them

All measured on a WT32-ETH01 with ESP-IDF v6.1. See `TESTING.md` for the tests.

### 1. Every frame is wrapped in an RFC 1042 header

802.11 data frames carry an LLC header. For Ethernet II frames the standard
header is RFC 1042 (`AA AA 03 00 00 00` + EtherType). For IPX (0x8137) and
AppleTalk AARP (0x80F3), 802.1H requires the "bridge-tunnel" header
(`AA AA 03 00 00 F8` + EtherType) instead, so that a receiver can tell Ethernet
II IPX apart from 802.2 SNAP IPX (which has `AA AA 03 00 00 00 81 37` as its
payload). 802.3 length frames (802.2, raw 802.3, SNAP) are sent with their own
payload as is.

The ESP32 does none of this. An over-the-air capture on an open network shows
it **always** sends `AA AA 03 00 00 00` followed by bytes 12-13 of the Ethernet
frame, even when those bytes are an 802.3 **length**:

```text
802.11 payload of an IPX frame sent from behind the ESP32:
aa aa 03 00 00 00  00 4a   aa aa 03 00 00 f8  81 37  ff ff ...
'- RFC 1042 hdr -'  'len'  '- frame payload (here a bridge-tunnel header)
```

The access point removes the RFC 1042 header and restores bytes 12-13, so
length frames still arrive on the wire unchanged. There is no software in the
ESP-IDF Wi-Fi libraries that builds these headers, so it is done by the
hardware or its firmware and cannot be changed through the normal transmit
path. Consequences:

| Sending from behind WifiRetroGate | arrives on the wire as |
|---|---|
| IPX 802.2, raw 802.3, SNAP | unchanged |
| IPX Ethernet II | **SNAP** (the access point applies 802.1H to RFC 1042 + 0x8137) |

**Firmware workaround:** for Ethernet II IPX/AARP from the client it sends the
frame as is (other machines get SNAP) **plus** a copy rebuilt as an 802.3
length frame with a bridge-tunnel header. Another WifiRetroGate converts that
copy back into Ethernet II; everyone else's IPX stack ignores the format it is
not configured for. Ethernet II IPX therefore works between WifiRetroGates.
For everything else, 802.2 is the format that works everywhere; the firmware
shows a hint when the client sends Ethernet II IPX.

A real fix would need either an unknown hardware option or sending raw 802.11
frames (`esp_wifi_80211_tx`), which are not encrypted, with CCMP done in
software. Not attempted.

### 2. Bridge-tunnel frames are received unconverted

Receiving works the other way round: RFC 1042 + IPX arrives as SNAP (correct),
but a bridge-tunnel frame (Ethernet II IPX from a wired machine, sent by the
access point the 802.1H way) is delivered as an 802.3 frame that still starts
with `AA AA 03 00 00 F8`. The firmware converts such frames into Ethernet II.

### 3. Received 802.3 frames have their length field set to 0

802.3 length frames from Wi-Fi (802.2, raw 802.3, SNAP from a wired machine)
are delivered with the **length field set to 0**. Linux ignores the field; DOS
IPX stacks (Novell LSL/IPXODI) rely on it and drop the frame. This made a Doom
game fail in one direction. Frames from Wi-Fi carry no padding, so the firmware
writes `frame length - 14` into the field. The `counters` command shows how
often it happened (`length repaired`).

## Result: IPX support

| IPX encapsulation | client -> others | others -> client |
|---|---|---|
| 802.2 (ODI default) | yes | yes |
| raw 802.3 (`IPX.COM`) | yes | yes |
| SNAP | yes | yes |
| Ethernet II | arrives as SNAP (except at another WifiRetroGate) | yes |

Other Ethernet II traffic (IPv4, IPv6, ARP, DECnet, ...) is not affected; only
the two EtherTypes 802.1H singles out are.

## Service address and Telnet console

The console must be reachable from the retro machine without any settings and
without WifiRetroGate having an IP address or MAC of its own in the LAN.

```text
retro machine: telnet 192.0.2.1
  192.0.2.1 is not in the local subnet -> send to the default gateway:
  frame dst MAC = router, dst IP = 192.0.2.1
        |
        v
WifiRetroGate: IPv4 from the client to the service address?
  yes -> hand the packet to a small lwIP interface (Telnet server), not to Wi-Fi
  reply -> dst MAC = client, src MAC = router (as learned from the request)
        |
        v
retro machine: an ordinary reply from 192.0.2.1 via the gateway
```

`192.0.2.0/24` is reserved for documentation (RFC 5737) and never used on real
networks. The address is configurable. Nothing in the LAN can reach it, which
keeps the password-less console private to the retro machine. The Telnet server
runs in line mode (the client echoes and sends whole lines), which suits DOS
Telnet clients.

### Gateway stand-in when no Wi-Fi was ever reached

The service address needs the client to reach its gateway, so with Wi-Fi down
it would be unreachable. When every saved network has been tried since
start-up and none was reached, WifiRetroGate answers the client's ARP requests
(not probes or announcements) with its own Ethernet MAC. The client then sends
its packets for `192.0.2.1` to that MAC, and they are picked out by
destination IP as usual. Up to 4 answered addresses are remembered. When Wi-Fi
connects, WifiRetroGate sends ARP requests for them on Wi-Fi with the client's
MAC and IP (twice, a second apart); the real replies reach the client and
replace the stand-in MAC in its ARP cache. After the first Wi-Fi connection
the stand-in is off until the next restart: a router restart or a short
dropout is not covered, since the real gateway comes back. No addresses are
ever handed out in bridge mode.

## Setup mode

The ESP32 runs an access point (`WifiRetroGate-XXXX`, WPA2, a random
per-device password shown on the display) with the following. If no display
answers at start-up, nobody could read that password, so the access point is
open instead.
- a DNS server answering every name with `192.168.4.1`, and an HTTP server that
  redirects every unknown URL to the setup page, so phones open it by
  themselves (captive portal);
- a one-page form: networks in range, add (with password or open), forget,
  finish. Hidden networks can be typed in by name (but see the known gap
  under Wi-Fi management).

Forwarding is off in setup mode, but the Ethernet port stays on so the retro
machine can do the setup itself:
- raw config frames (EtherType 0x88B5) go to the console as in bridge mode, so
  `WRG.COM` and `wrg.py` work;
- everything else goes to an lwIP interface of its own with the service
  address `192.0.2.1/24` and a DHCP server (`192.0.2.x`, 2-hour leases,
  gateway `192.0.2.1`). `telnet 192.0.2.1` therefore works the same as in
  bridge mode. This interface exists only in setup mode, when nothing is
  bridged, so it never competes with a real DHCP server. The lease is 2 hours
  because mTCP refuses to start with less than an hour left. After setup the
  machine needs a new address: run DHCP again (the `reboot` message says so);
  clients that renew by themselves get a NAK from the real router at the
  latest when the lease runs out.

Switching between setup and bridge mode is done with a restart. Setup mode
goes back to bridge mode after 10 minutes without use (no station on the
access point, no Telnet session, no console command) when at least one
network is saved; with none saved it would only come straight back.

## Wi-Fi management

- Up to 8 saved networks. Scan, try the saved networks in range strongest
  first, then the remaining saved ones, and repeat forever. Known gap: hidden
  networks (not in the scan) are only tried when at least one other saved
  network is visible, so a hidden network on its own is not found yet.
- WPA2/WPA3 (the threshold WPA2 accepts WPA3 and mixed networks), open
  networks. Failure reasons are shown (`wrong password`, `not found`, ...).
- Block-ack windows 32 (TX) / 16 (RX) and 240 MHz CPU for throughput; dynamic
  frequency scaling down to 80 MHz when idle, without light sleep and without
  Wi-Fi power saving.

## Memory and buffers

Free RAM is about 114 KB idle and dropped to 25 KB during a full-speed
transfer (Wi-Fi buffers are allocated on demand). Buffer counts are therefore
not raised further: little headroom, and longer queues add latency, which
matters more for games than the last Mbit/s.

## Firmware structure

| File | Purpose |
|---|---|
| `main.c` | boot: settings, display, button, console, mode choice, restart helper |
| `bridge.c` | Ethernet (LAN8720), client MAC and IP learning, forwarding, ESP32 workarounds, Ethernet in setup mode |
| `wifi_sta.c` | station manager: scan, choose, connect, reconnect, status |
| `setup.c` | setup access point, web page, captive-portal DNS, idle timeout |
| `svcnet.c` | service address: lwIP interface fed by the bridge |
| `telnet.c` | Telnet server (line mode) |
| `console.c` | command interpreter shared by serial, Telnet and raw frames |
| `serial_console.c` | console on UART0 |
| `rawcfg.c` | raw-frame configuration protocol (see `CONFIG-PROTOCOL.md`) |
| `settings.c` | saved networks, client MAC, setup AP password (NVS) |
| `display.c`, `status.c` | status screens, burn-in protection, status line |
| `oled.c`, `font5x8.c` | text-only SSD1306/SH1106 driver, 5x8 font |
| `button.c`, `eventlog.c` | setup button, event log for the `log` command |
