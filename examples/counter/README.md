# Counter Example (esp-tinybft)

A replicated counter over ESP-NOW or UDP (Wi-Fi): 7 replicas (f = 2) and 1
client on ESP32-C3. The client sends `increment` requests; the replicas order
and execute them and reply. This example is also the test bed for the
ESP-NOW vs UDP energy comparison.

## Quick start

Run everything from `examples/counter/` in a shell where `idf.py` works.

```bash
./gen_configs.sh                                         # keys + config files
cp sdkconfig.wifi.local.example sdkconfig.wifi.local     # UDP only: SSID/password (git-ignored)
./build_variant.sh espnow replica                        # -> build_espnow_replica/
./build_variant.sh espnow client                         # -> build_espnow_client/
python3 tools/flash.py espnow                            # every connected board gets its image
```

- **Node list.** MAC addresses (ESP-NOW) and IP addresses (UDP) of the eight
  boards live in the two here-documents inside `gen_configs.sh`; edit them
  there and rerun the script. It rewrites `spiffs_image/config_*.txt` and
  keeps the existing keys unless you pass `--keys`.
- **Do not use a bare `idf.py build`.** Role, transport and power-save mode are
  no longer in `sdkconfig.defaults`; they come from the overlays that
  `build_variant.sh` selects. A bare build would use whatever an old local
  `sdkconfig` says.

## Variants

`./build_variant.sh <variant> <replica|client> [idf.py args]` builds into
`build_<variant>_<role>/` with its own `sdkconfig`.

| Variant | Transport | Radio |
|---|---|---|
| `espnow` | ESP-NOW, station interface | always on |
| `espnow_ps` | ESP-NOW, station interface | receiver duty-cycled (wake window / interval) |
| `espnow_bcast` | ESP-NOW, one broadcast frame per protocol broadcast | always on, no ACK |
| `udp_psnone` | UDP | `WIFI_PS_NONE`: always on, the direct counterpart of `espnow` |
| `udp_psmin` | UDP | `WIFI_PS_MIN_MODEM`: wakes every DTIM |
| `udp_psmax` | UDP | `WIFI_PS_MAX_MODEM`: wakes every listen interval (3) |

All variants share `sdkconfig.defaults` (protocol sizes, UDP receive mailbox
16 = ESP-NOW queue depth) and `sdkconfig.measure` (log level WARN, statistics).
Both config files carry the same protocol timeouts.

Optional overlays are added with `EXTRA` and get their own build directory:

```bash
EXTRA="sdkconfig.pm" ./build_variant.sh udp_psmax replica
EXTRA="sdkconfig.pm;sdkconfig.static_ip" ./build_variant.sh udp_psmin replica
EXTRA="sdkconfig.mcast" ./build_variant.sh udp_psnone replica
```

| Overlay | Effect |
|---|---|
| `sdkconfig.pm` | CPU frequency scaling + automatic light sleep (see below) |
| `sdkconfig.usblog` | keep the USB console alive on a power-managed board (not for measured boards) |
| `sdkconfig.static_ip` | UDP: take the board's IP from its MAC, no DHCP reservation needed |
| `sdkconfig.mcast` | UDP: IP multicast instead of one datagram per peer |

## Deploying replicas and the client

The two roles are separate images. A board's role is fixed by the node list:
entries named `node*` are replicas, any other entry (here `client7`) is a
client. The board finds its own entry by MAC (ESP-NOW) or IP (UDP).

```bash
python3 tools/flash.py <variant>                 # read each board's MAC, pick the image
python3 tools/flash.py <variant> --dry-run       # show the plan only
python3 tools/flash.py <variant> --client-port /dev/ttyACM7   # no MAC read: that port = client
```

`flash.py` reads the MAC with esptool (this resets the board), looks it up in
`spiffs_image/config_espnow.txt` and writes bootloader, partition table,
application and the SPIFFS image. Boards whose MAC is not in the list are
skipped and reported. The same tool takes a sweep point id instead of a
variant name. `./flash_replicas.fish <variant>` is a wrapper around it.

To flash one board by hand:

```bash
./build_variant.sh <variant> replica -p /dev/ttyACM0 flash
./build_variant.sh <variant> client  -p /dev/ttyACM7 flash
```

After flashing, every board prints a `BANNER` line at boot. Check on each
board that `role=` is what you expect and that all boards show the same
transport, power-save and rate settings. A board with the wrong image logs
`wrong firmware role for this board?` and restarts in a loop instead of
sitting idle.

All eight boards must run the same variant. ESP-NOW station and softAP
builds cannot talk to each other.

## What the firmware prints

All three lines are at WARN level, so they survive the measurement overlay.

- `BANNER role=… transport=… …` once at boot: every setting that defines the
  experiment point (power save, listen interval, rate, retries, wake window
  and interval, broadcast/multicast, channel, power management, tick rate).
- `CLIENT_STATS ok=… fail=… missed=… last_ms=… min_ms=… avg_ms=… max_ms=…`
  after every request. The client sends on a fixed period; `missed` counts
  slots skipped because the previous request was still running.
- `TBFT_STATS …` every 60 s on replicas and after each request on the client:
  messages sent and received, send failures, queue drops, executed requests,
  re-sent commits, view changes, state fetches, free heap and its minimum.

## Power management (`sdkconfig.pm`)

The overlay follows Espressif's recommended automatic light-sleep
configuration for ESP32-C3 (ESP-IDF "Low Power Mode" guide): power
management, tickless idle, 1000 Hz tick, GPIO disabled in sleep, MAC/baseband
and CPU powered down in light sleep. `main.c` configures 160 MHz maximum,
crystal frequency minimum, light sleep enabled. The build uses about 3 KB
more static RAM than a build without it, plus roughly 4 KB of heap for the
MAC/baseband backup (Kconfig help); watch `heap_min` in `TBFT_STATS`.

- It only helps when the radio is allowed to sleep: `udp_psmin`, `udp_psmax`
  or `espnow_ps`. With an always-on radio the Wi-Fi driver holds a power lock
  and nothing changes.
- The replica sleeps until a message arrives or `TBFT_IDLE_WAIT_MS` (500 ms)
  elapses, so the CPU has long idle periods to sleep in.
- **The USB Serial/JTAG port stops answering while the chip is in light
  sleep.** Logs over USB are lost, and the board may need BOOT + RESET to be
  flashed again. Take logs from the UART0 pins with a USB-UART adapter, or
  build one logging board with `sdkconfig.usblog` and leave that board out
  of the energy figures.

## Best-vs-best sweep

Goal: tune each transport over its own free parameters, then compare the best
point of each by efficacy = successful requests per joule.

```bash
python3 tools/sweep.py plan --stage 1          # then 2, 3, 4 (see below)
python3 tools/sweep.py list
python3 tools/sweep.py build --only <point> …  # images in build_sweep/<point>_<role>/
python3 tools/flash.py <point>
python3 tools/analyze.py runs/
```

| Stage | Points | Purpose |
|---|---|---|
| 1 | ESP-NOW at 1M, 6M, MCS0, 24M, 54M, MCS7; UDP `PS_NONE` | always-on screen; choose the ESP-NOW rate |
| 2 | ESP-NOW at `--rates`: wake window/interval 50/100, 25/100, 25/200 ms with CPU sleep; 50/100 without; always-on with 8 retries | ESP-NOW power saving |
| 3 | UDP `PS_MIN`, `PS_MAX` (listen 3 and 10) with CPU sleep; minimum active time 20 ms; `PS_MIN`/`PS_MAX` without CPU sleep | UDP power saving |
| 4 | ESP-NOW broadcast at `--rates`; UDP multicast (`PS_NONE`, `PS_MIN` + CPU sleep) | sends without ACK |

Procedure:

1. Stage 1 needs no power meter. With an always-on radio the idle radio
   dominates the energy, so the PHY rate barely changes requests per joule.
   Choose the rate by success rate and latency; `analyze.py` prints the two
   fastest rates that pass and the stage 2 command.
2. Screen stages 2-4 with one short run per point, then repeat the two best
   points of each transport at least three times. `analyze.py` marks a result
   with fewer than three runs as a screen.
3. Save each run as `runs/<point>-r1/` (`-r2`, …) with one serial log per
   board (`node0.log` … `node6.log`, `client7.log`; only the client log is
   required) and `energy.csv` (`node,joules[,seconds]` or
   `node,avg_ma,volts,seconds`). Start the meter at the first successful
   request. Measure the same boards in every run.
4. Optional baseline: the same image with the client switched off, saved as
   `runs/<point>-base/`, gives the energy added per request.

`analyze.py` drops runs with view changes or state fetches (`--include-faulty`
keeps them), requires `--min-success` (default 95%), and says when the two
best points are within one standard deviation.

Keep fixed for every point: channel (set `EXAMPLE_ESPNOW_CHANNEL` to the
router's channel), router DTIM and beacon interval, request period, run
length, board positions. Interleave the points over time. The router's energy
is not counted on the UDP side.

What "best" means here: the best point among those you ran. The search is
staged, not exhaustive, and the request period is part of the result; repeat
at a second period if the ranking matters at other loads. `espnow_ps`
depends on retries landing inside the receiver's wake window and has not
been run on hardware.
