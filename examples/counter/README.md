# Counter Example (esp-tinybft)

A replicated counter over ESP-NOW or UDP (Wi-Fi): 7 replicas (f = 2) and 1
client on ESP32-C3. The client sends `increment` requests; the replicas order
and execute them and reply. This example is also the test bed for the
ESP-NOW vs UDP energy comparison.

Every setting of an experiment is chosen in `idf.py menuconfig`. No script is
needed to configure, build or flash. Why the experiment is set up this way
(presets, loads, and what was wrong with the earlier `devlope/v0.2.9` runs) is
in [docs/EXPERIMENT_DESIGN.md](../../docs/EXPERIMENT_DESIGN.md) (Thai).

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

### Experiment preset (the easy way)

*Counter Example Configuration > Experiment preset* sets a whole
configuration of the [best-vs-best comparison](#best-vs-best-configurations-to-run)
in one choice. The name before the colon is the run name, and the BANNER
prints it as `preset=`.

Every mode that worked on the boards has a preset. The "Measured" column is
from the older client, one request every 10 s:

| Preset | Transport | Radio setting | Measured (2026-11-15) |
|---|---|---|---|
| `en1` | ESP-NOW | 1 Mbps, receive power saving Off | 29/29, 2.37 s |
| `en65` | ESP-NOW | 65 Mbps (MCS7), Off | 30/30, 1.60 s |
| `en1_t8` | ESP-NOW | 1 Mbps, Off, 8 attempts per frame | 29/29, 2.41 s |
| `en1_w50i100` | ESP-NOW | 1 Mbps, listen 50 ms every 100 ms | 29/29, 2.95 s |
| `en1_w110i200` | ESP-NOW | 1 Mbps, listen 110 ms every 200 ms | 29/29, 2.79 s |
| `en1_w50i200` | ESP-NOW | 1 Mbps, listen 50 ms every 200 ms | 27/28, 4.32 s |
| `en65_w50i100`, `en65_w110i200`, `en65_w50i200` | ESP-NOW | 65 Mbps with the same three windows | not run yet |
| `udp_none` | UDP | power saving None | 29/29, 1.64 s |
| `udp_min` | UDP | Minimum | 29/29, 2.16 s |
| `udp_max3` | UDP | Maximum, listen interval 3 | 29/29, 2.30 s |
| `udp_max10` | UDP | Maximum, listen interval 10 | 29/29, 4.17 s |
| `udp_min_a20` | UDP | Minimum, minimum active time 20 ms | 28/28, 2.09 s |
| `udp_max3_a20` | UDP | Maximum, listen interval 3, minimum active time 20 ms | 29/29, 2.31 s |
| `udp_none_mcast` | UDP | None, IP multicast | 29/29, 1.90 s |
| `udp_min_mcast` | UDP | Minimum, IP multicast | 27/27, 2.29 s (one request 11.1 s) |
| Custom (default) | set by hand | | |

What a preset fixes, besides the column above: Wi-Fi interface = Station,
ESP-NOW broadcast = off, attempts per frame = 3 (8 for `_t8`), UDP multicast
= off (on for `_mcast`), and Wi-Fi *Minimum active time* = 50 ms (20 ms for
`_a20`). In the TinyBFT Configuration menu only the preset's option is then
offered for Transport, PHY rate, Wi-Fi interface and Receive power saving.

A preset leaves to you the settings that must stay the **same** in every run:
role, ESP-NOW channel, Wi-Fi TX power, timeouts and sizes.

Notes:

- *Minimum active time* is an ESP-IDF option that ESP-IDF applies only
  inside `esp_wifi_init()`. A UDP preset sets its own value with the same
  call (`esp_wifi_set_sleep_min_active_time()`, from `esp_private/wifi.h`)
  right after `esp_wifi_init()`, and the BANNER prints the value in effect
  (`min_active_ms=`). ESP-IDF's option in *Component config > Wi-Fi* then has
  no effect; with Custom it applies as before. This call has not been run on
  the boards yet.
- `_mcast` is a separate "one-to-many" point: a group frame has no ACK and no
  retry, so do not mix it into the unicast comparison.
- No preset exists for 6, 6.5, 11, 24 and 54 Mbps or for ESP-NOW broadcast:
  they did not work on the boards. They stay under *Custom*, and so does a
  custom wake window.
- Going back to Custom keeps the values the last preset left.

*Experiment load*, next to it, sets how the client sends for any preset:
*Back-to-back* (default; the next request as soon as the previous one
completes) or *One request every 10 s*. Together they give every mode, ESP-NOW
and UDP, at both loads; the BANNER prints `preset=` and `load=`.

Changing configuration is then: `idf.py menuconfig` → pick the preset and the load → save →
`idf.py build` → `./flash_all.fish`.

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
| Experiment preset | Custom (default), `en1`, `en65`, `en1_t8`, `en1_w50i100`, … `udp_min_mcast` | [above](#experiment-preset-the-easy-way); fixes the radio settings of one configuration |
| Experiment load | Back-to-back (default), One request every 10 s | how the client sends; BANNER `load=b2b` / `load=p10` |
| Role of this board | Automatic (default), Replica, Client | Automatic: from the node list |
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
- `CLIENT_STATS ok=… fail=… missed=… last_ms=… min_ms=… avg_ms=… max_ms=… value=…`
  after every request. `value` is the counter in the last accepted reply (-1
  before the first). The replicas execute a request once 2f+1 of them commit
  it, whether or not the client then gets f+1 matching replies, so when
  `value` jumps by more than `ok` the replicas executed requests the client
  counted as failed; with the replicas on batteries and no serial logs this
  is the only view of how many requests were agreed on. With load *Back-to-back* the next request starts as
  soon as the previous one has its reply (or failed), one request outstanding,
  and `missed` stays 0. With *One request every 10 s* the client keeps a fixed
  schedule and `missed` counts slots skipped because the previous request was
  still running. The BANNER prints `load=b2b` or `load=p10` and
  `req_interval_ms` (0 for back-to-back).
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
lists. The names are suggestions; keep one name per configuration. Every
name in the table has an *Experiment preset* of the same name (one choice in
menuconfig), except the rates and broadcast that did not work on the boards
(6, 6.5, 24, 54 Mbps, `_bcast`; 2026-11-15) and `en65_t8`; build those under
*Custom*.

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

1. Step 1: choose one or two rates by success rate and latency. With the
   back-to-back load a lower latency means more requests in the same energy;
   with one request every 10 s the idle radio dominates and the rate barely
   changes requests per joule.
2. Screen steps 2-4 with one short run per configuration, then repeat the two
   best configurations of each transport at least three times.
3. Save each run as `runs/<name>-r1/` (`-r2`, …) with one serial log per
   board (`node0.log` … `node6.log`, `client7.log`) and `energy.csv`
   (`node,joules[,seconds]` or `node,avg_ma,volts,seconds`). Measure the
   same boards in every run.
   - A run lasts from power-on until the batteries are empty, and all of it
     is data: boot, key exchange, the requests that fail while the cluster
     comes up and the last ones before a board browns out. Do not trim the
     warm-up. Make the start the same in every run instead: switch all
     boards on together, or keep one plug-in order and note when each board
     got power (the Commander CSV shows each channel's bus voltage rise).
     Measured on 2026-11-21 with the boards plugged in one by one, the client
     failed 9 requests and got its first reply 3 min 19 s after boot, about
     1 min after the last replica had power.
   - The client can stay on USB between runs while only the replicas run on
     batteries. Then start the client's log (or the client logger) when the
     batteries go in: a log that starts without the client's `BANNER` is
     counted from its first `CLIENT_STATS`, so the client's earlier runs do
     not count.
   - Joules from INA219 samples (Commander-Logger): the mean of bus
     voltage × current over all samples of the span, times the span's
     duration (first to last timestamp). Every sample is the average of one
     conversion period and the periods are equal, so the samples weigh the
     same; the timestamps are read times and jitter by about 20 ms. Do not
     multiply the sample count by a fixed period (the sensors deliver about
     15.0 results/s on the boards, 3 % faster than the datasheet's typical
     14.57; its ADC clock is 500 kHz ±30 %), and do not use the `power`
     column: the INA219 computes it from the same two registers and stores
     it in steps of 20 × the current LSB, so the product is never worse.
     Exports made before Commander `deb38ce` (±320 mV range) have 1.95 mW
     steps, 2.5-3 % of a power-saving board's power; since then (±80 mV)
     0.49 mW.
   - The client log is required and must include the boot `BANNER`. It gives
     success, latency and, from the view of the replies it accepted, any view
     change.
   - Keep the logs of all seven replicas, from boot: only they show a state
     fetch, a reboot (a second `BANNER`) or a replica that joined late.
   - The client never stops sending, so the run ends when you stop it:
     stop the meter and the logs at the same time, and run at least 60 s
     past the first request so every replica has printed `TBFT_STATS`
     (every 60 s).
   - Run every configuration under both loads (*Experiment load*), as
     separate points: `<name>` (back-to-back) and `<name>_p10` (10 s).
4. Optional baseline: the same image with the client switched off, saved as
   `runs/<name>-base/`, gives the energy added per request.

Keep fixed for every configuration: channel (an ESP-NOW channel away from the
AP, see above), router DTIM and beacon interval, run length,
board positions. Interleave the configurations over time. The
router's energy is not counted on the UDP side.

What "best" means here: the best configuration among those you ran. The
search is staged, not exhaustive, and the load is part of the result: at
full load (back-to-back) requests per joule show the cost of one consensus
round and power saving hardly gets to sleep; at one request every 10 s most
of the energy is idle time, which is what power saving saves. A configuration
that wins at one load need not win at the other, so report both.

### Optional: summarising runs

`python3 tools/analyze.py runs/` reads `CLIENT_STATS`, `TBFT_STATS`, the
`BANNER` (for the settings of each configuration) and `energy.csv`, and prints
per configuration: run length, the warm-up (client uptime at its first
success, requests failed before it), success rate, average and maximum
latency, requests per joule, view changes, state fetches, re-sent messages
and drops, then the best configuration per transport and whether the
difference exceeds one standard deviation. It requires `--min-success`
(default 95%) and is not needed for configuring, building or flashing.

It counts the whole run, power-on to the end: every request since the client
booted (counters summed when it reboots), every success's latency, the first
one after boot included, and replica counters since boot. View changes, state
fetches and reboots are counted and listed, not dropped: a replica whose
battery empties first forces a view change, and that is part of the result.
`--exclude-faulty` drops such runs instead. It notes a replica that joined
late, runs without replica logs (a state fetch cannot be detected then),
replica stats printed before the run ended and an `energy.csv` whose span
differs from the client's run by more than 5 %, and warns when a request
other than the first after boot comes close to the client re-send interval.

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
