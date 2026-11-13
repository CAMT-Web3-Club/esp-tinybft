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
- **Target.** `sdkconfig.defaults` sets `CONFIG_IDF_TARGET="esp32c3"` (the way
  the ESP-IDF `idf.py` guide gives a project a default target), so every new
  build directory is configured for ESP32-C3 without `set-target`.

## Variants

`./build_variant.sh <variant> <replica|client> [idf.py args]` builds into
`build_<variant>_<role>/` with its own `sdkconfig`.

| Variant | Transport | Radio |
|---|---|---|
| `espnow` | ESP-NOW, station interface | always on |
| `espnow_ps` | ESP-NOW, station interface | receive window every wake interval, RF off in between ([below](#esp-now-power-saving-espnow_ps)) |
| `espnow_bcast` | ESP-NOW, one broadcast frame per protocol broadcast | always on, no ACK |
| `udp_psnone` | UDP | `WIFI_PS_NONE`: always on, the direct counterpart of `espnow` |
| `udp_psmin` | UDP | `WIFI_PS_MIN_MODEM`: wakes every DTIM |
| `udp_psmax` | UDP | `WIFI_PS_MAX_MODEM`: wakes every listen interval (3) |

All variants share `sdkconfig.defaults` (target, protocol sizes, UDP receive
mailbox 16 = ESP-NOW queue depth) and `sdkconfig.measure` (log level WARN,
statistics). Both config files carry the same protocol timeouts. The CPU runs
at a fixed 160 MHz in every variant.

Optional overlays are added with `EXTRA` and get their own build directory:

```bash
EXTRA="sdkconfig.static_ip" ./build_variant.sh udp_psmin replica
EXTRA="sdkconfig.mcast" ./build_variant.sh udp_psnone replica
```

| Overlay | Effect |
|---|---|
| `sdkconfig.static_ip` | UDP: take the board's IP from its MAC, no DHCP reservation needed |
| `sdkconfig.mcast` | UDP: IP multicast instead of one datagram per peer |
| `sdkconfig.pm` | CPU frequency scaling + automatic light sleep. **Not part of the best-vs-best comparison** ([below](#cpu-power-management-sdkconfigpm-outside-the-comparison)) |
| `sdkconfig.usblog` | with `sdkconfig.pm` only: keep the USB console alive (not for measured boards) |

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
  experiment point (power save, listen interval, minimum active time, rate,
  retries, retry delay and span, wake window and interval,
  broadcast/multicast, channel, power management, tick rate).
- `CLIENT_STATS ok=… fail=… missed=… last_ms=… min_ms=… avg_ms=… max_ms=…`
  after every request. The client sends on a fixed period; `missed` counts
  slots skipped because the previous request was still running.
- `TBFT_STATS …` every 60 s on replicas and after each request on the client:
  messages sent (`tx`) and frames handed to the radio (`tx_frames`: every
  ESP-NOW attempt, fragments and retries included; one per UDP datagram),
  messages received, send failures, queue drops, executed requests, re-sent
  commits, view changes, state fetches, free heap and its minimum.

## ESP-NOW power saving (`espnow_ps`)

`espnow_ps` uses ESP-NOW's own power saving, which ESP-IDF calls
"connectionless module power save". Everything below is taken from ESP-IDF
v6.1: `esp_now.h`, `esp_wifi.h`, the Wi-Fi guide section "Connectionless
Modules Power-saving" and `examples/wifi/espnow`.

- After `esp_now_init()`, `main.c` calls `esp_now_set_wake_window(WINDOW)` and
  `esp_wifi_connectionless_module_set_wake_interval(INTERVAL)`, the same calls
  in the same order as `examples/wifi/espnow`. Station interface only;
  `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` is selected automatically, as
  the boards are never connected to an AP.
- At the start of every interval the RF, PHY and baseband turn on for
  `WINDOW` ms and are off for the rest of it. The CPU keeps running: this is
  modem sleep, not light sleep.
- Sending works at any time; only receiving waits for the window.
- Without the two calls the window stays at its maximum and the RF is always
  on. The `espnow` variant relies on that and also sets `WIFI_PS_NONE`, as
  the always-on receiver in Espressif's esp-now `coin_cell_demo` does.

ESP-IDF leaves window synchronisation between boards to the application. A
sender therefore retries a frame every `TBFT_ESPNOW_TX_RETRY_DELAY_MS`
(shorter than the window, so an attempt cannot skip over it) until the
receiver acknowledges it or `TBFT_ESPNOW_TX_RETRY_SPAN_MS` has passed (one
interval plus 20 ms in the sweep). Espressif's coin-cell button application
note (esp-now repository,
`examples/coin_cell_demo/switch/docs/button_in_matter_bridging.md`) uses the
same rule against an ESP-NOW bridge that listens 110 ms of every 200 ms:
attempts 50 ms apart, three of them, to cover the 90 ms the bridge sleeps.
Two things differ here: the button light-sleeps between attempts and the
bridge is connected to an AP, whereas these boards wait with the CPU running
and no AP is involved.

ESP-IDF documents one way to line windows up: connect sender and receiver to
the same AP and send on `WIFI_EVENT_CONNECTIONLESS_MODULE_WAKE_INTERVAL_START`.
It needs an AP, so it is not used.

Cost: every protocol step may wait up to (interval - window) for its receiver
to wake, so latency grows, and a missed window costs another attempt.
`espnow_ps` has not been run on hardware yet.

For scale, the ESP32-C3 datasheet (section 5.6, peak values at 3.3 V) gives
84 mA for receiving, 335 mA for sending at 1 Mbps, 285 mA at 54 Mbps and, in
modem sleep at 160 MHz, 16 mA with the CPU idle and 23 mA with it running
(all peripheral clocks off). Board-level figures are higher.

## CPU power management (`sdkconfig.pm`, outside the comparison)

The best-vs-best comparison uses radio modes only. No sweep point applies
`sdkconfig.pm`; every board runs its CPU at a fixed 160 MHz without light
sleep. The overlay stays for separate experiments.

It follows Espressif's automatic light-sleep configuration for ESP32-C3
(ESP-IDF "Low Power Mode" guide): power management, tickless idle, 1000 Hz
tick, GPIO disabled in sleep, MAC/baseband and CPU powered down in light
sleep. `main.c` then configures 160 MHz maximum, crystal frequency minimum,
light sleep enabled.

- It only helps when the radio may sleep (`udp_psmin`, `udp_psmax`,
  `espnow_ps`). With an always-on radio the Wi-Fi driver holds a power lock.
- **The USB Serial/JTAG port stops answering in light sleep.** Logs over USB
  are lost, and the board may need BOOT + RESET to be flashed again. Take logs
  from the UART0 pins, or build one logging board with `sdkconfig.usblog` and
  leave it out of the energy figures.

## Best-vs-best sweep

Goal: tune each transport over its own radio parameters, then compare the
best point of each by efficacy = successful requests per joule. Only radio
modes are swept: no point uses CPU light sleep or frequency scaling.

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
| 2 | ESP-NOW at `--rates`: wake window/interval 50/100 ms (ESP-IDF example), 110/200 ms (Espressif's ESP-NOW bridge), 50/200 ms, each with retry span = interval + 20 ms; always on with 8 attempts | ESP-NOW power saving |
| 3 | UDP `PS_MIN`; `PS_MAX` with listen interval 3 and 10; Wi-Fi minimum active time 20 ms (default 50) for `PS_MIN` and `PS_MAX` listen 3 | UDP power saving |
| 4 | ESP-NOW broadcast at `--rates` (default: IDF rate); UDP multicast with `PS_NONE` and `PS_MIN` | sends without ACK |

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
