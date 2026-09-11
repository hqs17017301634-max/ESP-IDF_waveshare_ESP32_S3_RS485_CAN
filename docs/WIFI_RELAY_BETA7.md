# S3 WiFi repeater beta.7 verification

Target: `waveshare_ESP32_S3_RS485_CAN`, ESP32-S3, ESP-IDF 6.0.1,
16 MiB DIO/80 MHz flash, 8 MiB PSRAM. Parent source: `3550ec9`.
This change retains the single-CAN frame coordinator and existing DNS domain policy.

## Changes

- Use `esp_netif_napt_enable/disable()` so NAT lifecycle operations run through
  ESP-NETIF's TCP/IP thread dispatch. Remove direct NAPT table mutation from the
  application task. Clear DNS cache and pending queries on upstream transitions.
- Check WiFi initialization, event registration, mode, IP/DHCP configuration,
  start and connection errors. Expose the last API error separately from the
  asynchronous disconnect reason. Keep a failed AP's saved credentials intact.
- Use RAM WiFi-driver storage; the dashboard remains the owner of saved NVS
  networks. Preserve full 32-byte SSIDs and 64-character hexadecimal WPA2 PSKs,
  reject invalid lengths instead of silently truncating them.
- Downstream AP uses WPA2-PSK/CCMP. Upstream WPA3 SAE configuration now uses
  `CONFIG_ESP_WIFI_ENABLE_WPA3_SAE`; the previous test mistakenly treated the SAE
  enum as a preprocessor macro. PMF remains capable, not mandatory.
- Report connected only after GOT_IP; clear the state on disconnect/lost IP.
  Following link loss, retry the last working saved network after 2 seconds;
  a failed attempt falls back to the existing saved-network rotation.
- DNS uses explicit nonblocking lwIP sockets and `lwip_poll`, bounded UDP
  batches, one retry at 1.2 seconds and SERVFAIL after the original 3-second
  deadline. Socket initialization failures retry with backoff.
- DNS validates question/class/flags, upstream source/port/ID/question and RR
  bounds. Coalescing requires identical query bytes excluding the ID. Cache TTLs
  follow the minimum record TTL, capped at 60 seconds, and age on each reply.
  EDNS/CD queries bypass the shared cache. OPT flags are not modified as TTLs.
- Handle local `t.sl` replies with EDNS correctly. Bound UDP query size to 1232
  bytes, response processing to 4096 bytes and emit TC when needed. A separate
  bounded TCP DNS worker applies the same local/AAAA/domain policy. It accepts
  one query per connection with a 3.5-second overall limit; it is not a general
  recursive resolver or a high-concurrency DNS server.
- Protect policy/cache/pending state with a recursive mutex. Socket waits occur
  outside it. CAN processing and NAT packet forwarding do not acquire it.
- Expose UDP receive/local/invalid/retry/socket error counters, TCP readiness and
  query count, plus the UDP worker heartbeat. A live heartbeat/bound socket is
  service readiness, not proof that a client's DNS request reached the device.

WiFi/TCP buffer counts were not enlarged: the available measurements do not
identify them as the throughput bottleneck.

## Offline validation

- 23 Python tests, 12 WebUI scenarios, 192 native tests in 12 suites: passed.
- 688,128 RX events across three CAN feature profiles: output identical to the
  migration baseline; coordinator contracts passed.
- Isolated target compile/link passed. Image info verified ESP32-S3, 16 MB,
  DIO/80 MHz, checksum and validation hash. The application descriptor reads
  `3550ec9-dirty`; exact source inputs are bound by the release manifest hashes.

## Module checks after OTA

The WebUI accepted the OTA with HTTP 200 `OK`. The rebooted module reports
`3.0.0-beta.7`, running app0 at `0x20000`.

- Both saved networks preserved; CAN writing and CAN priority remained off.
- System status returned all 59 fields. Manual scan returned 20 networks without
  requiring a CAN-priority toggle.
- 20 management requests passed: median 23.5 ms, maximum 156 ms. This is HTTP
  request latency, not packet loss or sustained relay bandwidth.
- Reconnecting the active saved network through `/wifi_connect` recovered in
  about 7 seconds with API error 0; NAT returned ready. This checks manual
  reconnect, not the automatic lost-phone fallback across four saved networks.
- Sample free internal memory 139,275 bytes, free PSRAM 7,674,220 bytes;
  CPU load samples were approximately 6% / 8%. These are snapshots, not a soak.

### DNS test boundary: Windows Clash TUN

Direct UDP DNS requests timed out before and after this OTA, while the module's
receive count stayed zero. TCP connect to port 53 failed immediately with
Windows `WSAEACCES` (10013); the same bound WiFi interface reached module port 80.
The PC runs Clash Verge/Mihomo with TUN, `strict-route: true` and `dns-hijack:
any:53`. Mihomo documents that strict routing on Windows adds DNS leak-prevention
firewall rules: <https://wiki.metacubex.one/en/config/inbound/tun/#strict-route>.

These observations strongly identify a PC-side DNS test obstruction. The zero
receive count is **not evidence that the old DNS worker was broken**, and changing
from select to poll is not claimed to have fixed that symptom. Direct UDP/TCP
DNS completion, local answers, cache hits and upstream timeout behavior still
require a client without that interception. PC proxy settings were preserved.

### Short relay download samples

Python/OpenSSL, certificate verification and SNI, explicit destination Host,
socket source `100.100.1.2` and WiFi interface 9, no HTTP proxy. DNS was resolved
with AliDNS HTTPS separately to avoid the PC's UDP-53 restriction. Each sample
validated HTTP 206, `Content-Range` and exactly 2,000,000 downloaded bytes from
the official Aliyun Ubuntu mirror. Payload was discarded. Rate includes TLS
and response startup overhead.

| Firmware / destination | Sample 1 | Sample 2 |
|---|---:|---:|
| beta.6, 117.162.32.54 | 2.05 Mbps | 2.72 Mbps |
| beta.7, same 117.162.32.54 | 2.99 Mbps | 2.76 Mbps |
| beta.7, alternate CDN 117.162.32.48 | 1.88 Mbps | 2.13 Mbps |

The same-server samples are slightly higher, but this is insufficient evidence
of a firmware throughput improvement. There was no simultaneous direct-upstream
baseline. Do not market the 150 Mbps PHY rate as repeater throughput. No iPhone
versus Android A/B, long-duration reconnect/roaming, or vehicle CAN-load test was
performed. S3 still requires a compatible **2.4 GHz** hotspot; it cannot join a
5 GHz-only hotspot.

## Images

Local output directory: `dist/s3-wifi-relay-beta7` (ignored by Git).

- `s3-wifi-relay-beta7_OTA.bin`: 1,132,832 bytes.
  SHA-256 `a13ecc6ce9396fcd0b2782ff63630ecaea14d945f67e10a199f7d3965a163d01`.
- `s3-wifi-relay-beta7_FULL_16MB.bin`: 16,777,216 bytes, serial flash offset `0x0`.
  SHA-256 `1dcefdcfd6f5136657e0ca2c5d9a255500d0d43b4ec1e007aeb6b64b48644db7`.

The merged image uses bootloader `0x0`, partition table `0x8000`, otadata
`0x19000`, app0 `0x20000`, verified against the actual partition table. All input
segments byte-match and unused ranges are FF. Full flash replaces NVS/SPIFFS
contents; it is a factory image, not a backup of the module. Only the OTA app
was uploaded. The preceding beta.6 images remain available locally for rollback.
