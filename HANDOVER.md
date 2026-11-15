# Handover: esp-tinybft review fixes and the first board runs

Updated 2026-11-15. Read `AGENT.md` (architecture and conventions; `CLAUDE.md`
is the same text), then `docs/REVIEW_2026-11-15.md` (the findings, with a fix
status table at the top).

## Project in one paragraph

`esp-tinybft` is a PBFT component for ESP-IDF 6.1 on ESP32-C3, written in C23
with static memory only (no malloc in the component). `examples/counter` runs
7 replicas + 1 client (f = 2) and is the testbed for a research experiment
comparing ESP-NOW and UDP energy use (successful requests per joule), "best
configuration vs best", using radio power modes only (no CPU light sleep).
Experiment settings live in `idf.py menuconfig`, not in custom scripts.

## Rules from the owner (keep them)

- Branch `fix/develop/v0.2.9`. Commit there and push. Do not open a PR or MR.
  End commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Correctness and stability first, security second. ESP32-C3 only.
- Never commit Wi-Fi credentials: they live only in the git-ignored
  `examples/counter/sdkconfig`. Do not print them, and do not build with that
  file: build with `-D SDKCONFIG=<other path>`.
- Do not regenerate RSA keys, and do not re-enable timestamp anti-replay.
- Check ESP-IDF behaviour against `~/.espressif/v6.1/esp-idf` first.
- Docs are Markdown; Thai is fine for docs aimed at the owner.

## Done (committed on `fix/develop/v0.2.9`)

Groups 1 and 2 of the review, plus the two transport fixes found on the
boards:

- **Measurement (X2–X7):** timeouts come from menuconfig (the config file's
  lines are unused and written as 0); the BANNER prints them; the Status timer
  is periodic; `run_*` statistics count from the first executed request; every
  re-send and drop is counted; `analyze.py` excludes faulty runs, warns about
  missing replica logs and about requests slower than the client re-send
  interval; broadcast is only selectable with receive power saving off.
- **Liveness (L1–L4, L6, L7, L9, R1, R2):** View_change re-sent every Status
  period; no escalation when cut off; fetch aims at the newest f+1-vouched
  checkpoint and re-announces its own Checkpoint; `prep_view` reset by
  `nv_install`; read-only queries do not take over slots; duplicate Meta_data
  not queued twice; stale vtimer bits ignored; Fetch index bounds checked.
- **ESP-NOW send path (X8):** one frame in flight, waiting for its send
  callback (2 s safety net), and duplicate frames from a sender are dropped.
  New counters: `cb_fail`, `cb_timeout`, `send_err`, `cb_slow`, `cb_max_ms`,
  `rx_dup`.
- **Docs:** counter README, AGENT.md, ARCHITECTURE.md, FIX_PLAN (including a
  §6 table of these fixes and the board results), REVIEW status table.
  `sizeof(tbft_replica_t)` is 79,112 bytes; `memcalc.sh` matches.

## Verified on the boards (8 ESP32-C3 SuperMini, ESP-NOW defaults, 1 Mbps)

- Plain 8-minute run: 48/48 requests, no view change, no reboot, heap_min
  44–46 KB, zero failed sends. Before the X8 fix: 43/44, 4.4–40 KB heap,
  15–630 failed sends, steady-state latency 4.9 s (now 2.0 s).
- Fault run: resetting node0 (primary) produced a clean view change to view 1
  and node0 fetched back to the current state; resetting node3 (backup) caused
  no view change and it caught up. All replicas ended at `view=1
  last_exec=27 last_stable=26`, 27 client successes, no skipped seqno.

## Not done

1. **Simulator runs** (review §"ทำซ้ำผลใน simulator"): the previous agent's
   runs never started (a zsh word-splitting bug in its wrapper), and the
   owner stopped the simulator work in favour of the boards. Run them before
   trusting L1/L2/L4/L7 under loss:
   `python3 tests/sim/run.py --seed 104 --env SIM_LOSS=0.15 S3b` etc. (run in
   a clean environment, or the IDF environment breaks the host gcc).
2. **New simulator scenarios** S3f (kill two replicas while one fetches) and
   duplicate datagrams (`SIM_DUP`), for L2 and L7.
3. **Other board configurations:** 54 Mbps + power saving, broadcast +
   Replica, UDP + PS max + multicast. They build; only ESP-NOW defaults has
   been run on hardware.
4. **`--san address,undefined`** simulator pass.
5. **Remaining review items:** L5, L8, R3–R11, and the X1 value of *Client
   request retransmission interval* (measure `max_ms` of the slowest
   configuration; 5 s is enough for ESP-NOW defaults, whose steady state is
   2.4 s).
6. **Hardware note:** three boards were connected through a USB hub that
   repeatedly failed to enumerate them (`error -110/-71`, flashing timeouts).
   They now plug straight into the motherboard; the other hub (a Genesys
   05e3:0610) never failed. Keep the boards off that hub.

## Helper scripts (outside the repo)

They live in `../esp-tinybft-work` (next to the repo) and are not committed:
`build.sh <example> <name> [fragments]`, `flash.sh <build> [ports]`,
`run_all.sh <run> <seconds>`, `faulttest.sh <run> <s> <t1> <t2>`,
`reset_board.py`, `capture.py`, `hwsum.py`, `hwcmp.py`, `idfbin/env.sh`.
Builds go to `builds/<name>` with their own sdkconfig, so the owner's
`sdkconfig` is never touched. Logs go to `hw/runs/<run>/<node>.log`.
`idfbin/env.sh` exports the EIM-layout ESP-IDF 6.1 environment (the venv is
`~/.espressif/tools/python/v6.1/venv`, not the path `export.sh` expects).
