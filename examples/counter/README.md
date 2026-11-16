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
| UDP: send protocol broadcasts as one datagram per replica | **on (default)**; off = IP multicast | UDP only. Unicast is the counterpart of ESP-NOW unicast-per-peer (ACKed, retried); a multicast group frame has no ACK, is sent at a low basic rate and a station in power saving may not hear it. Measured: unicast 29/29 in every power-saving mode, multicast 29/29 and 27/27 (one request at 11.1 s). Use multicast only as a separate "one-to-many" point |
| ESP-NOW radio > Wi-Fi interface | Station (default), SoftAP | keep Station: no beacons, and power saving needs it |
| ESP-NOW radio > PHY rate | 1 (ESP-IDF default), 11, 6, 6.5 (MCS0), 24, 54, 65 (MCS7) Mbps | per peer, `esp_now_set_peer_rate_config()`. Measured on 8 boards (ESP-IDF 6.1): only **1 Mbps and 65 Mbps (MCS7)** are usable; 6, 6.5, 11, 24 and 54 Mbps lose 73–100 % of the MAC ACKs (`docs/FIX_PLAN_COUNTER_ENERGY_FAIRNESS.md` §6) |
| ESP-NOW radio > Send protocol broadcasts as one broadcast frame | off (default), on | on: no ACK and no retry. A message that needs more than one frame (New_key) is sent unicast even then. Measured: broadcast still stalls at a seqno, so it is not usable yet |
| ESP-NOW radio > Receive power saving | Off (default), 50 ms every 100 ms, 110 ms every 200 ms, 50 ms every 200 ms, Custom | [below](#esp-now-receive-power-saving) |
| ESP-NOW radio > Wake window / Wake interval | only shown for Custom | ms |
| ESP-NOW radio > Attempts per frame | 3 | minimum number of attempts |
| ESP-NOW radio > Delay between attempts | 10 ms | only with power saving Off |
| Protocol sizes, Timeouts | | keep them fixed for the comparison (set in `sdkconfig.defaults`: view change 30 s, Status 5 s, state fetch 10 s, client re-send 6 s, client reply 20 s). The timeout lines at the end of the config files are not used (`gen_configs.sh` writes 0 there) |
| Wi-Fi TX power | 0 = driver default (20 dBm), or 0.25 dBm units (32 = 8 dBm) | both transports. **Boards on one desk desense each other at 20 dBm**: measured with 8 boards 10–20 cm apart, ESP-NOW lost 90–100 % of its MAC ACKs and never finished the key exchange, while the same boards at 8 dBm lost none. Space the boards out or lower this, and keep it the same for every configuration |

### Counter Example Configuration (experiment)

| Setting | Choices | Notes |
|---|---|---|
| Role of this board | Automatic (default), Replica, Client | Automatic: from the node list |
| Client: request period (ms) | 10000 | the same for every configuration you compare |
| ESP-NOW Wi-Fi channel | 1 | use a channel **away** from the AP: on the AP's channel 1 Mbps lost 27 % of its ACKs and the receive power-saving modes stopped working |
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
  tick rate), plus the protocol timeouts in ms: `vc_ms` (view change),
  `status_ms`, `recovery_ms` (state fetch), `retx_ms` (client re-send) and
  `reply_ms` (client reply).
- `CLIENT_STATS ok=… fail=… missed=… last_ms=… min_ms=… avg_ms=… max_ms=…`
  after every request. The client sends on a fixed period; `missed` counts
  slots skipped because the previous request was still running.
- `TBFT_STATS …` every 60 s on replicas and after each request on the client.
  - Both: messages sent (`tx`) and frames handed to the radio (`tx_frames`:
    every ESP-NOW attempt, fragments and retries included; one per UDP
    datagram), sends given up (`tx_fail`), send-queue drops (`tx_qfull`),
    messages received (`rx`), receive drops (`rx_drop`, `rx_frag_drop`), free
    heap and its minimum (`heap`, `heap_min`).
  - ESP-NOW only (0 with UDP), what happened to each frame: no ACK
    (`cb_fail`), no send callback within the 2 s safety timeout
    (`cb_timeout`), refused by `esp_now_send()` (`send_err`), callbacks slower
    than 100 ms (`cb_slow`) and the slowest one (`cb_max_ms`). `rx_dup` counts
    frames dropped as an exact repeat of the previous frame from the same
    sender (its ACK was lost); they are not drops.
  - Replicas: `view`, `last_exec`, `last_stable`, executed requests, every
    message sent again (`resent`: re-sent commits, pre-prepares, prepares,
    checkpoints, new-views, cached replies, New_keys and View_changes;
    `commit_resent` counts the commits among them), view changes (`vc_sent`,
    `new_view`) and state fetches (`fetch`). `first_exec` is the sequence
    number of the first client request the replica executed; the `run_*`
    counters (`run_vc`, `run_nv`, `run_fetch`, `run_resent`, `run_tx_fail`,
    `run_drop`) count from there, so the boot phase does not show in them.
  - Client: the highest `view` of an accepted reply (a view change during the
    run shows here), received replies (`rx_reply`) and re-sent requests
    (`req_resent`).

  Nothing is re-sent on a timer in a healthy run: replicas re-send only what a
  peer reports missing in its Status, and the client re-sends a request only
  when it has no f+1 matching replies after *Client request retransmission
  interval* (TinyBFT Configuration > Timeouts, 5000 ms by default, doubling).
  Non-zero `resent` / `req_resent` mean that messages were lost and
  recovered, or that a request took longer than the re-send interval: compare
  `max_ms` in `CLIENT_STATS` with `retx_ms` in the BANNER (`tools/analyze.py`
  warns when the slowest request comes within 80 % of it). Keep the interval
  above the slowest configuration's `max_ms`, the same for every
  configuration.

The client keeps a boot counter in NVS (namespace `tbft`) so that its request
ids keep growing after a reset; the replicas ignore requests whose id is not
above the last one they executed. Erasing the client's flash
(`idf.py erase-flash`) resets that counter: restart all replicas as well in
that case (they keep their state, the client table included, in RAM only),
otherwise they keep ignoring the client.

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
   board (`node0.log` … `node6.log`, `client7.log`) and `energy.csv`
   (`node,joules[,seconds]` or `node,avg_ma,volts,seconds`). Start the meter
   at the first successful request. Measure the same boards in every run.
   - The client log is required and must include the boot `BANNER`. It gives
     success, latency and, from the view of the replies it accepted, any view
     change.
   - Keep the logs of all seven replicas, from boot: only they show a state
     fetch, a reboot (a second `BANNER`) or a replica that joined late.
   - Keep logging at least 60 s after the last request: replicas print
     `TBFT_STATS` every 60 s, and the last line must cover the whole run.
4. Optional baseline: the same image with the client switched off, saved as
   `runs/<name>-base/`, gives the energy added per request.

Keep fixed for every configuration: channel (an ESP-NOW channel away from the
AP, see above), router DTIM and beacon interval, request period, run length,
board positions. Interleave the configurations over time. The
router's energy is not counted on the UDP side.

What "best" means here: the best configuration among those you ran. The
search is staged, not exhaustive, and the request period is part of the
result; repeat at a second period if the ranking matters at other loads.

### Optional: summarising runs

`python3 tools/analyze.py runs/` reads `CLIENT_STATS`, `TBFT_STATS`, the
`BANNER` (for the settings of each configuration) and `energy.csv`, and prints
success rate, average and maximum latency, requests per joule, re-sent
messages and drops per run, the best configuration per transport and whether
the difference exceeds one standard deviation. It requires `--min-success`
(default 95%) and is not needed for configuring, building or flashing.

It drops a run (`--include-faulty` keeps it) when the client saw a view
change, a board rebooted, a replica joined late (`first_exec` > 1) or a
replica's `run_*` counters show a view change or a state fetch. It notes runs
without replica logs (a state fetch cannot be detected then), replica stats
printed before the run ended and an `energy.csv` that covers another span
than the requests, and warns when the slowest request comes close to the
client re-send interval.

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
