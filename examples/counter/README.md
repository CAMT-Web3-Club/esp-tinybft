# Counter Example (esp-tinybft)

A replicated counter over ESP-NOW or UDP (Wi-Fi): 7 replicas (f = 2) and 1
client on ESP32-C3. The client sends `increment` requests; the replicas order
and execute them and reply. This example is also the test bed for the
ESP-NOW vs UDP energy comparison.

Every setting of an experiment is chosen in `idf.py menuconfig`. No script is
needed to configure, build or flash.

## Quick start

Run these from `examples/counter/` in a shell where `idf.py` works.

```bash
./gen_configs.sh      # keys + node list (edit the MAC/IP lists inside first)
idf.py menuconfig     # choose transport, radio mode, ... (see below)
idf.py build
./flash_all.fish      # the same image to every connected board
```

- **Node list.** MAC addresses (ESP-NOW) and IP addresses (UDP) of the eight
  boards live in the two here-documents inside `gen_configs.sh`; edit them
  there and rerun the script. It rewrites `spiffs_image/config_*.txt` and
  keeps the existing keys unless you pass `--keys`. The list is flashed with
  the image (SPIFFS partition).
- **One image for all boards.** With *Role of this board: Automatic* (the
  default) each board looks itself up in the node list at boot: entries
  named `node*` run as replicas, the others (here `client7`) as the client.
- **Coming from an older checkout.** Start once from a clean configuration so
  the new defaults apply: delete `sdkconfig` and `sdkconfig.old`, then run
  `idf.py fullclean`. After pulling Kconfig changes later, run
  `idf.py reconfigure` (or `idf.py menuconfig`) before building: a plain
  `idf.py build` may not pick them up.
- **Target.** `sdkconfig.defaults` sets `CONFIG_IDF_TARGET="esp32c3"` (the
  way the ESP-IDF `idf.py` guide gives a project a default target), so no
  `set-target` is needed.
- **Wi-Fi credentials** (UDP) are entered in menuconfig and stored in
  `sdkconfig`, which git ignores.

In menuconfig, `?` shows the help of the selected option, `f` toggles a help
panel, `/` searches and `r` restores an option's default.

## Where each setting lives

Two menus at the top level of `idf.py menuconfig` belong to this example.

### TinyBFT Configuration (radio)

| Setting | Choices | Notes |
|---|---|---|
| Transport | UDP over Wi-Fi, ESP-NOW (default here) | all boards the same |
| UDP: send protocol broadcasts as one datagram per replica | on (default here); off = IP multicast | UDP only |
| ESP-NOW radio > Wi-Fi interface | Station (default), SoftAP | keep Station: no beacons, and power saving needs it |
| ESP-NOW radio > PHY rate | 1 (ESP-IDF default), 6, 6.5 (MCS0), 24, 54, 65 (MCS7) Mbps | per peer, `esp_now_set_peer_rate_config()` |
| ESP-NOW radio > Send protocol broadcasts as one broadcast frame | off (default), on | on: no ACK and no retry |
| ESP-NOW radio > Receive power saving | Off (default), 50 ms every 100 ms, 110 ms every 200 ms, 50 ms every 200 ms, Custom | [below](#esp-now-receive-power-saving) |
| ESP-NOW radio > Wake window / Wake interval | only shown for Custom | ms |
| ESP-NOW radio > Attempts per frame | 3 | minimum number of attempts |
| ESP-NOW radio > Delay between attempts | 10 ms | only with power saving Off |
| Protocol sizes, Timeouts | | keep them fixed for the comparison (set in `sdkconfig.defaults`) |

### Counter Example Configuration (experiment)

| Setting | Choices | Notes |
|---|---|---|
| Role of this board | Automatic (default), Replica, Client | Automatic: from the node list |
| Client: request period (ms) | 10000 | the same for every configuration you compare |
| ESP-NOW Wi-Fi channel | 1 | use the router's channel |
| Wi-Fi station (UDP) > Wi-Fi SSID, Wi-Fi password | | stored in `sdkconfig` |
| Wi-Fi station (UDP) > Power saving | None (default), Minimum, Maximum | `esp_wifi_set_ps()` |
| Wi-Fi station (UDP) > Listen interval | 3 | Maximum only |
| Wi-Fi station (UDP) > Derive a static IP from this board's MAC | off | no DHCP reservation needed |

### In ESP-IDF's own menus

- *Component config > Wi-Fi > Minimum active time* (default 50 ms): how long
  a UDP station with Minimum or Maximum power saving stays awake after
  traffic.
- *Component config > Log > Log Level > Default log verbosity*: Warning in
  `sdkconfig.defaults`, so every configuration logs the same lines. Set Info
  for debugging.

## ESP-NOW receive power saving

This is ESP-NOW's own power saving, which ESP-IDF calls "connectionless
module power save". Everything below comes from ESP-IDF v6.1 (`esp_now.h`,
`esp_wifi.h`, the Wi-Fi guide section "Connectionless Modules
Power-saving", `examples/wifi/espnow`).

- When the ESP-NOW transport starts, right after `esp_now_init()`, TinyBFT
  calls `esp_now_set_wake_window(WINDOW)` and
  `esp_wifi_connectionless_module_set_wake_interval(INTERVAL)`: the same
  calls in the same place as `examples/wifi/espnow`. Station interface only;
  `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` is selected automatically,
  because the boards never connect to an AP.
- At the start of every interval the RF, PHY and baseband turn on for
  `WINDOW` ms and are off for the rest of it. The CPU keeps running: this is
  modem sleep, not light sleep.
- Sending works at any time; only receiving waits for the window.
- With power saving Off the window stays at its maximum and the RF is always
  on. The example also sets `WIFI_PS_NONE` then, as the always-on receiver in
  Espressif's esp-now `coin_cell_demo` does.

ESP-IDF leaves window synchronisation between boards to the application, so a
sender repeats every frame until the receiver acknowledges it: every half
window (whole FreeRTOS ticks) for at least one interval plus 20 ms. Because
the attempts are closer together than the window, one of them falls inside
it. With the 100 Hz tick:

| Receive power saving | Listens | Every | Sender retries every | for at least |
|---|---|---|---|---|
| 50 ms every 100 ms (ESP-IDF example defaults) | 50 ms | 100 ms | 20 ms | 120 ms |
| 110 ms every 200 ms (Espressif ESP-NOW bridge) | 110 ms | 200 ms | 50 ms | 220 ms |
| 50 ms every 200 ms | 50 ms | 200 ms | 20 ms | 220 ms |
| Custom | W | I | W/2, whole ticks | I + 20 ms |

A Custom window that is not shorter than the interval, or too short for the
tick, stops the build with a message saying so.

Espressif's coin-cell button application note (esp-now repository,
`examples/coin_cell_demo/switch/docs/button_in_matter_bridging.md`) uses the
same rule against an ESP-NOW bridge (esp-matter `esp-now_bridge_light`) that
listens 110 ms of every 200 ms: attempts 50 ms apart, three of them, to cover
the 90 ms the bridge sleeps. Two things differ: the button light-sleeps
between attempts and the bridge is connected to an AP, whereas these boards
wait with the CPU running and no AP is involved. The documented way to line
windows up (sender and receiver on the same AP, sending on
`WIFI_EVENT_CONNECTIONLESS_MODULE_WAKE_INTERVAL_START`) needs an AP and is
not used.

Cost: every protocol step may wait up to (interval - window) for its receiver
to wake, so latency grows, and a missed window costs another attempt. Receive
power saving has not been run on hardware yet.

For scale, the ESP32-C3 datasheet (section 5.6, peak values at 3.3 V) gives
84 mA for receiving, 335 mA for sending at 1 Mbps, 285 mA at 54 Mbps and, in
modem sleep at 160 MHz, 16 mA with the CPU idle and 23 mA with it running
(all peripheral clocks off). Board-level figures are higher.

## Flashing

With *Role: Automatic*, one build serves every board:

```bash
idf.py build
./flash_all.fish                             # every /dev/ttyACM* port, in parallel
./flash_all.fish /dev/ttyACM0 /dev/ttyACM5   # only these ports
```

`flash_all.fish` runs, for each port, the command `idf.py build` prints at
the end (`python -m esptool ... write-flash @flash_args` in `build/`) and
writes one log per port to `build/flash_logs/`. One board at a time works
too: `idf.py -p /dev/ttyACM0 flash`.

Forced roles (*Replica* or *Client*): build and flash the replica boards,
then switch the role in menuconfig, build and flash the client board. A board
whose node-list entry disagrees with a forced role says so and restarts.

After flashing, every board prints a `BANNER` line at boot (below). Check
that all eight boards show the same settings and that each `role=` is what
you expect. ESP-NOW station and softAP builds cannot talk to each other.

## What the firmware prints

All three lines are at WARN level, so they appear with the default log level.

- `BANNER role=… role_from=… id=… transport=… …` once at boot: the role and
  whether it came from the node list (`list`) or menuconfig (`forced`), the
  node id, and every setting that defines the configuration (power saving,
  listen interval, minimum active time, rate, attempts, retry delay and span,
  wake window and interval, broadcast/multicast, channel, power management,
  tick rate).
- `CLIENT_STATS ok=… fail=… missed=… last_ms=… min_ms=… avg_ms=… max_ms=…`
  after every request. The client sends on a fixed period; `missed` counts
  slots skipped because the previous request was still running.
- `TBFT_STATS …` every 60 s on replicas and after each request on the client:
  messages sent (`tx`) and frames handed to the radio (`tx_frames`: every
  ESP-NOW attempt, fragments and retries included; one per UDP datagram),
  messages received, send failures, queue drops, executed requests, re-sent
  commits, view changes, state fetches, free heap and its minimum.

## Best-vs-best: configurations to run

Goal: tune each transport over its own radio settings, then compare the best
configuration of each by efficacy = successful requests per joule. Only radio
modes are compared: no configuration uses CPU light sleep or frequency
scaling.

Start every configuration from the defaults and change only what the table
lists. The names are suggestions; keep one name per configuration.

| Step | Name | TinyBFT Configuration | Counter Example Configuration / ESP-IDF |
|---|---|---|---|
| 1 | `en1`, `en6`, `en6.5`, `en24`, `en54`, `en65` | Transport ESP-NOW; PHY rate 1 / 6 / 6.5 / 24 / 54 / 65 Mbps; Receive power saving Off | |
| 1 | `udp_none` | Transport UDP | Power saving None |
| 2 | `en<r>_w50i100` | ESP-NOW, PHY rate r; Receive power saving 50 ms every 100 ms | |
| 2 | `en<r>_w110i200` | as above, 110 ms every 200 ms | |
| 2 | `en<r>_w50i200` | as above, 50 ms every 200 ms | |
| 2 | `en<r>_t8` | ESP-NOW, PHY rate r; power saving Off; Attempts per frame 8 | |
| 3 | `udp_min` | Transport UDP | Power saving Minimum |
| 3 | `udp_max3`, `udp_max10` | Transport UDP | Power saving Maximum; Listen interval 3 / 10 |
| 3 | `udp_min_a20`, `udp_max3_a20` | Transport UDP | as `udp_min` / `udp_max3`; Minimum active time 20 |
| 4 | `en<r>_bcast` | ESP-NOW, PHY rate r; Send protocol broadcasts as one broadcast frame | |
| 4 | `udp_none_mcast`, `udp_min_mcast` | Transport UDP; "UDP: send protocol broadcasts as one datagram per replica" off | Power saving None / Minimum |

`r` is the PHY rate chosen in step 1.

Procedure:

1. Step 1 needs no power meter. With an always-on radio the idle radio
   dominates the energy, so the PHY rate barely changes requests per joule.
   Choose one or two rates by success rate and latency.
2. Screen steps 2-4 with one short run per configuration, then repeat the two
   best configurations of each transport at least three times.
3. Save each run as `runs/<name>-r1/` (`-r2`, …) with one serial log per
   board (`node0.log` … `node6.log`, `client7.log`; only the client log is
   required, and it must include the boot `BANNER`) and `energy.csv`
   (`node,joules[,seconds]` or `node,avg_ma,volts,seconds`). Start the meter
   at the first successful request. Measure the same boards in every run.
4. Optional baseline: the same image with the client switched off, saved as
   `runs/<name>-base/`, gives the energy added per request.

Keep fixed for every configuration: channel (set *ESP-NOW Wi-Fi channel* to
the router's channel), router DTIM and beacon interval, request period, run
length, board positions. Interleave the configurations over time. The
router's energy is not counted on the UDP side.

What "best" means here: the best configuration among those you ran. The
search is staged, not exhaustive, and the request period is part of the
result; repeat at a second period if the ranking matters at other loads.

### Optional: summarising runs

`python3 tools/analyze.py runs/` reads `CLIENT_STATS`, `TBFT_STATS`, the
`BANNER` (for the settings of each configuration) and `energy.csv`, and prints
success rate, latency, requests per joule, the best configuration per
transport and whether the difference exceeds one standard deviation. It drops
runs with view changes or state fetches (`--include-faulty` keeps them) and
requires `--min-success` (default 95%). It is not needed for configuring,
building or flashing.

## CPU power management (not part of the comparison)

Every configuration above runs the CPU at a fixed 160 MHz without light
sleep. `sdkconfig.pm` lists the settings for a separate light-sleep
experiment (the ESP32-C3 settings of the ESP-IDF "Low Power Mode" guide). To
try it without touching the main configuration, give it its own build
directory and `sdkconfig`:

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.pm" idf.py -B build_pm -D SDKCONFIG=build_pm/sdkconfig menuconfig
```

The USB Serial/JTAG port stops answering while the chip is in light sleep:
take logs from the UART0 pins, or add `sdkconfig.usblog` on one logging board
and leave that board out of the energy figures.
