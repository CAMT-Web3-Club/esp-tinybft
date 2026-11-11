# Counter Example (esp-tinybft)

This example demonstrates how to use `esp-tinybft` to build a simple
distributed counter application over ESP-NOW and UDP (Wi-Fi).

## Architecture

The state of the application is a single integer counter starting at 0.
Clients submit `increment` requests which increase the counter by 1.
Requests are ordered and agreed upon by the BFT replicas before being executed.

## Setup

1. **Generate Configuration and Keys**
   First, generate the RSA keys and configuration files that will be flashed onto the devices:
   ```bash
   ./gen_configs.sh
   ```
   This creates a `spiffs_image` directory containing:
   - RSA private and public keys (`privX.der`, `pubX.der`)
   - `config_udp.txt` and `config_espnow.txt`

2. **Configure the Transport**
   Use `idf.py menuconfig` to configure the transport mechanism and network settings:
   - Navigate to `Component config -> TinyBFT Configuration`
   - Select either **ESP-NOW** or **UDP (lwIP sockets)** under `Transport backend`.
   - If using UDP, navigate to `Counter Example Configuration` and set your Wi-Fi SSID and Password.

3. **Build and Flash**
   You need 8 ESP32 boards (7 replicas to tolerate 2 faults, and 1 client).

   Build and flash:
   ```bash
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Note on MACs and IP Addresses

The generated `config_espnow.txt` and `config_udp.txt` contain dummy MAC addresses and loopback IP addresses.
**For a real test across multiple boards, you MUST edit `config_espnow.txt` in the `spiffs_image/` folder**
to contain the real MAC addresses of your ESP32 boards before running `idf.py build`.

## Experiment variants (energy comparison)

Variants are built from overlay files, each into its own `build_<variant>_<role>/`
directory with its own `sdkconfig` (no `menuconfig`, no hand-edited sdkconfig):

| Variant | Transport | Radio |
|---|---|---|
| `espnow` | ESP-NOW (station interface) | always on |
| `espnow_ps` | ESP-NOW (station) | duty-cycled RX: window/interval, retries raised to span one interval (optional, untested on hardware) |
| `udp_psnone` | UDP | `WIFI_PS_NONE` (always on, direct counterpart of `espnow`) |
| `udp_psmin` | UDP | `WIFI_PS_MIN_MODEM` |
| `udp_psmax` | UDP | `WIFI_PS_MAX_MODEM`, listen interval 3 |

```bash
cp sdkconfig.wifi.local.example sdkconfig.wifi.local   # SSID/password, git-ignored
./build_variant.sh udp_psmax replica build
./build_variant.sh udp_psmax client build
./build_variant.sh espnow replica -p /dev/ttyACM0 flash
```

All variants share `sdkconfig.defaults` (protocol sizes/timeouts, UDP receive
mailbox = 16 to match the ESP-NOW queue) and `sdkconfig.measure` (log level WARN,
`TBFT_STATS`). `gen_configs.sh` writes the same timeouts to both config files and
keeps existing keys unless run with `--keys`.

Every boot prints a `BANNER` line (role, transport, PS mode, listen interval,
channel, request period) at WARN level - check it on every board before measuring.
The client prints `CLIENT_STATS` after each request and `TBFT_STATS` counters
(sent/received per message type, send failures, queue drops, view changes,
re-sent commits). Report runs with `vc_sent`/`fetch` > 0 or failed requests
separately.

Measurement notes: use the same channel for ESP-NOW (`EXAMPLE_ESPNOW_CHANNEL`)
and the router; UDP energy excludes the router; ESP-NOW and UDP `PS_NONE` are
the like-for-like pair; `PS_MIN`/`PS_MAX` save RF power only because the CPU
never sleeps (power management is not enabled).

Optional overlays, passed with `EXTRA` (they get their own build dir):

```bash
EXTRA="sdkconfig.pm" ./build_variant.sh udp_psmax replica build            # CPU DFS + light-sleep
EXTRA="sdkconfig.pm;sdkconfig.static_ip" ./build_variant.sh udp_psmin client build
```

- `sdkconfig.pm` works because the replica loop and the client now sleep in
  `tbft_transport_wait()` (select/semaphore) instead of polling every tick.
  Apply it to every variant of a comparison or to none.
- `sdkconfig.static_ip` derives the board IP from its MAC (no DHCP reservation).
- `TBFT_ESPNOW_PHY_RATE` sets the ESP-NOW rate (0 = IDF default 1 Mbps); report it.
