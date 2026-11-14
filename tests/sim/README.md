# TinyBFT host cluster simulator

Runs the esp-tinybft PBFT component as a cluster of Linux processes so that
protocol correctness and stability can be tested without ESP32 boards.

- 7 replicas + 1 client (f = 2), the counter example's application.
- Every node is its own process with its own loopback address
  `127.0.0.(11+id)` and UDP port, using the **real** `src/tbft_transport_udp.c`
  (unicast, `CONFIG_TBFT_DISABLE_MULTICAST`).
- Every `src/*.c` file except `tbft_principal.c` and
  `tbft_transport_espnow.c` is compiled **unmodified** with
  `gcc -std=gnu23 -Wall -Wextra`, against the headers in `shim/` (first on
  the include path).
- `stub_principal.c` replaces the crypto: no RSA, but key-exchange state
  (`hmac_in/out_key`, `keys_fresh`, New_key ciphertext caches) evolves as on
  hardware.

## Build and run

Requirements: gcc with `-std=gnu23` (gcc 14+; tested with gcc 16), GNU make,
Python 3.10+. Linux (uses the 127.0.0.0/8 loopback range).

```bash
make -C tests/sim                 # build/sim_replica, build/sim_client
python3 tests/sim/run.py          # build + all scenarios (~20 min, one after the other)
python3 tests/sim/run.py S1 S3    # selected scenarios
python3 tests/sim/run.py --list   # scenario list
```

Useful options of `run.py`:

| option | meaning |
|---|---|
| `--no-build` | skip `make` |
| `--seed N` | seed for start offsets, loss and node PRNGs (default 1) |
| `--repeat N` | run each scenario N times with seeds seed..seed+N-1 |
| `--log LEVELS` | `SIM_LOG` for every node, e.g. `INFO` or `WARN,tbft_replica=INFO` |
| `--auth strict\|lax` | stub verification mode (see below), default strict |
| `--vc-timeout-ms`, `--status-timeout-ms`, `--recovery-timeout-ms` | config-file timeouts (default 3000/500/1000; hardware uses 30000/5000/10000) |
| `--client-boot-delay-ms` | client delay before `Byz_init_client` (default 5000, as the example) |
| `--quiesce-s` | wait after the client finished, before the final check (default 10) |
| `--env NAME=VALUE` | extra environment for every node (e.g. `SIM_LOSS=0.02`) |
| `--san address,undefined` | build in `build-san/` with sanitizers and run that |
| `--out DIR` | run directory root (default `tests/sim/runs/`) |

Each invocation creates `<out>/<YYYYmmdd-HHMMSS>/<scenario>/` containing
`config.txt`, the key files, `node0.log` .. `node6.log`, `client.log`,
`events.log` (what the orchestrator did and when), `result.json`, and
`summary.json` one level up. All times in logs are seconds since the
scenario started (same clock in every process).

### Running nodes by hand

```bash
cd some/dir   # config.txt, pub<i>.key, priv<i>.key (key = any bytes, same in both files)
SIM_T0_NS=$(date +%s%N) ../tests/sim/build/sim_replica 3 config.txt
../tests/sim/build/sim_client 7 config.txt <period_ms> <count> [--start-n N] [--boot-delay-ms MS]
```

Config format is the one `parse_config()` reads (same as
`examples/counter/gen_configs.sh`); paths in it must be shorter than 128
characters (`%127s`), so `run.py` uses paths relative to the run directory.

## Scenarios

| name | what happens | expectation |
|---|---|---|
| S1 | all replicas start together, ordered so every replica is listening before node0's first New_key; client: 40 requests every 500 ms | all accepted, no view change |
| S1r | all replicas start together in id order (node0 first, as a shell loop or a simultaneous power-on would do it) | all accepted |
| S2a | replicas start at seeded random offsets in 0-30 s; client starts first | all accepted |
| S2b | same offsets; client starts after the last replica (t = 32 s) | all accepted |
| S3 | node0 (primary of view 0) is SIGKILLed after 10 accepted requests and stays down; 60 requests | requests accepted after the crash |
| S3b | node0 and node1 (primaries of views 0 and 1) are SIGKILLed together after 10 accepted requests; 60 requests | requests accepted after the crash (view 2) |
| S3r | node0 is SIGKILLed after 10 accepted requests and restarted 15 s later; 80 requests | requests accepted after the crash, node0 ends with the majority state |
| S4 | node3 is SIGKILLed after 15 accepted requests and restarted 20 s later; 120 requests; 15 s quiescence | node3 ends with the majority state |
| S5 | the client is SIGKILLed after 15 accepted requests and restarted 3 s later with the remaining requests (new incarnation; its request ids continue through the NVS boot epoch, as after a board reset) | every request after the restart accepted |
| S5p | as S5, then node0 is SIGKILLed 3 requests after the client restart | requests accepted after the crash |
| S6a / S6b | 5 % / 10 % of all datagrams (every node) dropped | invariants hold |

The client is cut off after a time budget (about 170 s per scenario); the
requests it never sent are reported as "unsent".

## Invariants

Checked after the client is done and a quiescence period, from the logs:

- **(a) state agreement** - every live replica has the same counter value and
  SHA-256 of the 4096-byte state. Reported as groups `counter@[nodes]`;
  `FAIL(safety)` if two replicas have the same `last_exec` but different
  state, `FAIL(lag)` otherwise.
- **(b) exactly-once** - with `ok`/`fail`/`inflight` the client's accepted,
  failed (timed out) and killed-while-pending requests:
  `ok <= counter <= ok + fail + inflight`, and, using a per-request token the
  sim client puts after the op byte and the replica logs at every execution
  (`SIM_EXEC`): no request executed twice, no accepted request missing, each
  counter value and each sequence number executed with one request on every
  replica, every accepted reply value equal to the executed value, and the
  majority counter equal to the number of distinct executed requests.
- **(c) protocol counts** - view changes sent, new views installed, state
  fetches started (from the replicas' counters and the event log lines),
  highest view.
- **(d) skipped sequence numbers** - log lines where a replica moved
  `last_executed` past a seqno without executing it:
  `not committed (skipped by view-change), continuing`,
  `no pre-prepare ... continuing`, `commit digest mismatch ... skipping`,
  `advancing window head`. `FLAG` if any.
- **liveness** - the scenario's expectation from the table above.
- **health** - unexpected process exits (crash, `esp_restart()`, a task
  returning; the node is restarted after 1 s like a rebooting board), task
  watchdog reports, sanitizer reports.

## How it works

`shim/` provides, on top of glibc and pthreads:

| header | implementation |
|---|---|
| `sdkconfig.h` | counter example values: MAX_NUM_REPLICAS 7, MAX_MESSAGE_SIZE 2048, MAX_REPLY_SIZE 900, BLOCK_SIZE 1024, WINDOW_SIZE 4, CHECKPOINT_INTERVAL 2, RQUEUE_MAX 4, MAX_STATE_BLOCKS 8, P_LEVELS 2, NDET 256, CLIENT_REPLY_TIMEOUT_MS 20000, MAX_NUM_CLIENTS 1, IDLE_WAIT_MS 500, PRINT_STATS, UDP, DISABLE_MULTICAST, FREERTOS_HZ 1000, task WDT 10 s. Override with `make EXTRA_CFLAGS=-DCONFIG_...=...` |
| `freertos/*.h` | tasks = pthreads (a returning task ends the process like ESP-IDF's abort), `vTaskDelay`, tick count at `CONFIG_FREERTOS_HZ`, event groups, semaphores, queues |
| `esp_timer.h` | one "esp_timer task" thread; callbacks run concurrently with the tasks; `esp_timer_get_time()` = monotonic us since process start (0 at every boot, like hardware) |
| `esp_log.h` | `[run_s] L (boot_ms) tag: msg` on stdout; level from `SIM_LOG`; protocol event messages (view changes, fetches, skipped seqnos, ...) always printed |
| `esp_system.h` | `esp_restart()` exits with code 86 (the orchestrator reboots the node) |
| `esp_task_wdt.h` | ESP-IDF's "Task watchdog got triggered" report plus a `SIM_WDT` line, no panic |
| `esp_netif.h`, `lwip/*.h` | host sockets; `bind(INADDR_ANY)` is narrowed to the node's own 127.0.0.x so the transport's local-id detection and the reply source address work as on a board; `sendto()` fault injection; `select()` releases the task lock |
| `psa/crypto.h` | software SHA-256 behind `psa_hash_*` |
| `mbedtls/pk.h` | a key = SHA-256 fingerprint of the key file |
| `esp_random.h` | PRNG seeded from `SIM_SEED`, node id and boot count (a rebooted node draws new session keys) |
| `nvs.h` | `nvs_open/get_u32/set_u32/commit/close` backed by one small file per key in the node's working directory, so the client's boot epoch survives a restart |

The replica process runs `Byz_init_replica()` + `Byz_replica_run()` in a
FreeRTOS task (as `examples/counter/main/main.c` does); its main thread
prints a `SIM_REPLICA` status line every second. It finds the static
`tbft_replica_t` through the esp_timer named `tbft_vtimer` (whose argument
is `&replica->vtimer`), and reads it under a task lock that the replica task
only releases while blocked (`select`, `vTaskDelay`, event-group waits), so
the snapshot is never taken half-way through a message handler.

### Principal stub (`stub_principal.c`)

- MACs are HMAC-SHA256 under the real session keys; New_key "encryption"
  carries the key with the recipient's key fingerprint and random padding
  (so re-encryptions differ, as with RSA-OAEP); signatures are 256 bytes
  derived from the signer's key fingerprint and the signed bytes.
- `SIM_AUTH=strict` (default) verifies MACs with the installed in-key and
  signatures with the claimed signer's key, so a message MACed under a key
  the receiver has not installed yet, or modified after it was MACed or
  signed, is rejected exactly as on hardware. `SIM_AUTH=lax` makes every
  verification succeed.
- `SIM_SIGN_MS`, `SIM_VERIFY_MS`, `SIM_ENC_MS`, `SIM_DEC_MS` (default 0) add
  a busy wait per operation to approximate the ESP32-C3's RSA cost.

### Environment variables read by the node processes

| variable | default | meaning |
|---|---|---|
| `SIM_LOG` | `WARN` | log levels, `LEVEL[,tag=LEVEL...]` |
| `SIM_LOG_EVENTS` | 1 | 0 = do not force-print protocol event lines |
| `SIM_LOSS` | 0 | drop probability per datagram (sender side) |
| `SIM_LOSS_PER_FRAGMENT` | 0 | 1 = apply `SIM_LOSS` per 1472-byte IP fragment |
| `SIM_LOSS_FROM_S`, `SIM_LOSS_UNTIL_S` | always | loss only in this run-time window |
| `SIM_SEED`, `SIM_INCARNATION` | 1, 0 | PRNG seeding (orchestrator sets them) |
| `SIM_T0_NS` | process start | run start (CLOCK_REALTIME ns) for log times |
| `SIM_AUTH` | strict | see above |
| `SIM_STATUS_MS` | 1000 | `SIM_REPLICA` line period |
| `SIM_NO_NVS` | 0 | 1 = `nvs_open()` fails, as with an uninitialised NVS partition |

## Known limitations

- **Timing is not the ESP32-C3's.** Crypto takes microseconds (unless the
  `SIM_*_MS` knobs are used), the loopback delivers in microseconds and never
  reorders, and the scenarios use 10x shorter config-file timeouts than
  `gen_configs.sh` (3 s view change instead of 30 s) while the compile-time
  client timeout stays 20 s and the key-exchange periods in `tbft_replica.c`
  (300 ms stagger, 3 s barrier re-broadcast, 60 s barrier timeout, 5 s / 60 s
  re-broadcast) are hard-coded. Ratios between timeouts therefore differ from
  hardware; use `--vc-timeout-ms 30000 --status-timeout-ms 5000
  --recovery-timeout-ms 10000` for the hardware values (slow).
- **Network model.** Loss only (random, sender side). No reordering,
  duplication, delay, partitions, IP fragmentation or Wi-Fi contention; the
  kernel socket buffer is much larger than lwIP's 16-entry UDP mailbox
  (`CONFIG_LWIP_UDP_RECVMBOX_SIZE`), so receive-overflow drops do not happen.
- **One node per process**, as `tbft_libbyz.c` keeps the replica/client in
  static singletons. Stack size, heap size (`heap=` in TBFT_STATS is a
  200 kB model minus malloc'd bytes) and the 64-bit host ABI differ from the
  RISC-V target; a stack overflow or a 32-bit-only bug would not show here.
- **No crypto and no Byzantine behaviour** - all nodes are correct; faults
  are crashes, reboots, late starts and loss.
- **Data race freedom is only as good as the target's**: timer callbacks run
  on their own thread (as the esp_timer task preempts the replica task on
  hardware), so the races between `vtimer_cb`/`stimer_cb` and the replica
  task that exist on hardware exist here too.
