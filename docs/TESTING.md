# Testing

## IPX encapsulation test

`tools/ipxtest.py` sends IPX broadcast frames in all four encapsulations
(Ethernet II, 802.2, raw 802.3, SNAP); each frame's payload names the format it
was sent in. The listener reports `OK` (arrived in the same format),
`CONVERTED` (arrived in another format), or `BAD` (the 802.3 length field is
missing or wrong, which DOS IPX stacks reject). A missing line means the frame
was dropped.

```text
                       +---------------+
 Linux laptop ---------| WifiRetroGate | ~ ~ ~ Wi-Fi ~ ~ ~ [AP] ---- wired Linux machine
 (USB Ethernet,        +---------------+                    (switch)
  plays the retro machine)

 Test 1, WifiRetroGate -> wire:
   wired:   sudo python3 ipxtest.py listen eth0
   laptop:  sudo python3 ipxtest.py send <usb-ethernet-interface>

 Test 2, wire -> WifiRetroGate:
   laptop:  sudo python3 ipxtest.py listen <usb-ethernet-interface>
   wired:   sudo python3 ipxtest.py send eth0
```

Each run sends 3 rounds of 4 frames, so 12 lines are expected. Afterwards, the
WifiRetroGate's `counters` command shows the workarounds that were applied.

Also worth testing: Wi-Fi to Wi-Fi (listen on another Wi-Fi client of the same
access point), and two WifiRetroGates.

Note: a listener on Wi-Fi must have power saving off
(`sudo iw dev <wifi-interface> set power_save off`), or it misses broadcasts.

## Results (WT32-ETH01, ESP-IDF v6.1, WPA3 home access point)

| Encapsulation | WifiRetroGate -> wire | wire -> WifiRetroGate |
|---|---|---|
| Ethernet II | CONVERTED (arrives as SNAP) | OK (bridge-tunnel converted by firmware) |
| 802.2 | OK | OK (length field repaired by firmware) |
| raw 802.3 | OK | OK (length field repaired) |
| SNAP | OK | OK (length field repaired) |

Real games: **Doom** (`IPXSETUP -nodes 2`) between a DOS PC with a VIA Rhine
card behind WifiRetroGate (Novell ODI, no `NET.CFG`, so 802.2) and a wired DOS
PC: starts and plays well.

Other results:

- DHCP, IPv4, IPv6 (router advertisements, i.e. multicast), HTTPS: work.
- Throughput through the bridge (Internet speed test): 27.7 Mbit/s down,
  30 Mbit/s up (18.5 / 24.5 with ESP-IDF default settings).
- A 6502 machine's open Telnet session survived moving its cable from the wired
  LAN to a WifiRetroGate.
- Telnet console on `192.0.2.1`, setup mode from a phone, OLED display: work.
- Setup mode from a 6502 machine on the cable: DHCP (`192.0.2.x`),
  `telnet 192.0.2.1`, `wifi add`, `reboot`, then connected to that network:
  works. The machine's IP address appears on the display.
- An open Telnet session from the 6502 machine survived reflashing the
  firmware (the board comes back with the same cloned MAC).
- Setup mode from DOS with mTCP: `DHCP`, then `telnet 192.0.2.1`: works (mTCP
  needs a lease of at least an hour, so setup mode gives 2 hours). Typed
  characters echo in mTCP Telnet and in the 6502 machine's Telnet.
- `WRG.COM` in setup mode: works. DOOM over IPX in bridge mode: works.

## Over-the-air capture

Seeing what the ESP32 actually sends needs a capture in monitor mode on an
**open** test network (on WPA2/WPA3 the payload is encrypted):

1. Another computer runs an open access point on a fixed 2.4 GHz channel, for
   example with NetworkManager:

   ```sh
   nmcli con add type wifi ifname <wifi> con-name WRGTEST autoconnect no ssid WRGTEST \
       802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel 6 \
       ipv4.method shared
   nmcli con up WRGTEST
   ```

2. WifiRetroGate joins it (`wifi add WRGTEST`, forget the others, `reconnect`).
3. A Wi-Fi adapter in monitor mode on the same channel records it:

   ```sh
   sudo iw dev <mon> set type monitor; sudo ip link set <mon> up
   sudo iw dev <mon> set channel 6
   sudo dumpcap -i <mon> -w /tmp/air.pcap
   ```

   Then filter for the WifiRetroGate's frames, for example
   `tshark -r /tmp/air.pcap -Y 'wlan.ta == <client-mac> && wlan.fc.type == 2'`.

Adapters: a Realtek RTL8821AU USB stick with the in-kernel `rtw88_8821au`
driver (Linux 6.17) worked well. An Intel BE201 produced only malformed frames
in monitor mode. Raspberry Pi built-in Wi-Fi does not support real monitor mode
with the stock firmware.

## BOOTP

`tools/bootptest.py IFACE` sends a plain BOOTP request (no DHCP options, as
Trumpet Winsock 2.x does) and reports whether anything answers. The home router
tested did not answer, which is common.
