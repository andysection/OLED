# ESP32 Classic Bluetooth PANU for iPhone

For the complete Chinese implementation guide and troubleshooting checklist,
see [PANU_IMPLEMENTATION_GUIDE.md](PANU_IMPLEMENTATION_GUIDE.md).

The ESP32 acts as a Classic Bluetooth PAN User (PANU). It discovers an iPhone,
connects to the iPhone's Network Access Point (NAP), obtains an IPv4 address
through DHCP or an IPv6 address through SLAAC, and uses the iPhone's Personal
Hotspot as its Internet connection.

```text
ESP32 application <-- lwIP (DHCP/SLAAC) <-- Bluetooth BNEP <-- iPhone <-- Internet
```

## Configure the iPhone name

On the iPhone, open **Settings > General > About > Name**. Put that exact name
in `sdkconfig.defaults`:

```ini
CONFIG_PAN_PEER_DEVICE_NAME="Liyao's iPhone"
```

The comparison is exact and case-sensitive. After changing defaults, remove an
old generated `sdkconfig.esp32dev` or run `pio run -t menuconfig` and
change **Bluetooth PANU > iPhone Bluetooth device name** there.

## Build and flash

The public `esp_pan_*` API is newer than ESP-IDF 5.5, so the PlatformIO project
uses pinned framework revisions. This checkout currently points to locally
cached framework and toolchain paths in `platformio.ini`; update those paths or
install equivalent package revisions when moving the project to another machine.

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## Connect to iPhone

1. Enable Bluetooth on the iPhone.
2. Open **Settings > Personal Hotspot** and enable **Allow Others to Join**.
3. Keep the Personal Hotspot or Bluetooth settings page open during discovery.
4. Reset the ESP32. It scans for the configured iPhone name automatically.
5. Accept the pairing request on the iPhone and confirm the displayed number.

After the first pairing, if the ESP32 has exactly one bonded Classic Bluetooth
device, it reconnects to that device directly. This allows the firmware to keep
retrying every 15 seconds when Personal Hotspot is off without requiring the
iPhone to remain discoverable. If several devices are bonded, it scans by the
configured iPhone name instead.

After pairing, the log should show `connected to iPhone PAN`, followed by an
IPv4 or routable IPv6 address assigned by the iPhone. The firmware pings
`8.8.8.8` on IPv4. On IPv6-only PAN it directly tests Cloudflare's IPv6 NTP
address `2606:4700:f1::123`, avoiding a dependency on DNS64. On disconnect, the ESP32
prints a Personal Hotspot reminder, waits 15 seconds, and then retries. If the
PAN link does not obtain either usable address type within 30 seconds, the ESP32
disconnects and retries. Static IPv4 fallback is disabled by default because it
cannot replace the iPhone's DHCP authorization and may create an address
conflict. While online, it repeats the Internet ping test every 30 seconds and
prints `Internet test PASSED` or `Internet test FAILED`. It also synchronizes
China Standard Time from Cloudflare's IPv6 NTP service, prints it immediately
after synchronization, and prints the current time every 30 seconds while the
PAN network remains online.

Each time a usable network path appears, the firmware also sends an HTTP GET
and a JSON HTTP POST to `httpbin.org`. The serial log reports the HTTP status
code or the transport error, which distinguishes an ICMP-only failure from a
general TCP/Internet failure.

The retry, address timeout, IPv4/IPv6 network test targets, time server, time
zone, and print interval are configurable under **Bluetooth PANU** in
`pio run -t menuconfig`.

If the iPhone router advertisement does not include an RDNSS option, the
firmware uses Alibaba Public DNS at `2400:3200::1` as its IPv6 fallback DNS.

Apple can change Bluetooth tethering behavior between iOS releases. If the
iPhone does not advertise its name, first select and pair `ESP32_PANU` from the
iPhone Bluetooth settings while the ESP32 is discoverable, then retry with the
Personal Hotspot screen open.
