# AGENT.md

This file provides guidance to AI agents (Claude Code, Gemini CLI, etc.) when working with code in this repository.

## Build commands

This is an **ESP-IDF component** (not a standalone app). It is built as part of a host project that adds it as a managed component or local component.

### Local IDF build

```bash
# From a host ESP-IDF project that uses this component:
idf.py build                   # full build
idf.py build 2>&1 | head -50   # check for early errors
idf.py menuconfig              # configure TBFT_* parameters (TinyBFT Configuration menu)
idf.py flash monitor           # flash and open serial console
idf.py -p /dev/ttyUSB0 flash monitor  # with explicit port

# Component-only sanity check (requires IDF env active):
idf.py set-target esp32c3
idf.py build
```

### Docker build (espressif/idf image)

Use the Espressif Docker image when local IDF toolchain is unavailable. The image contains ESP-IDF, cross-compiler toolchains, CMake, Ninja, and all Python dependencies.

**Standard Docker invocation pattern:**

```bash
docker run --rm -it \
  -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp \
  espressif/idf:v6.0.1 \
  <idf.py command>
```

Key flags:
- `-it` — interactive with colors and progress bars
- `-u $(id -u)` — run as host user so build artifacts have correct ownership
- Mount repo root at `/esp-tinybft` consistently

**Common commands:**

```bash
# One-time: create symlink so IDF finds the component
mkdir -p examples/counter/components
ln -sfn ../../.. examples/counter/components/esp-tinybft

# Clean + set target + build:
docker run --rm -it -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py fullclean
docker run --rm -it -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py set-target esp32c3 && idf.py build

# Quick build check (component only):
docker run --rm -it -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py build 2>&1 | head -50

# Flash (requires --device for USB access):
docker run --rm -it --device /dev/ttyACM0 \
  -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py -p /dev/ttyACM0 flash

# Serial monitor (user runs this directly):
docker run --rm -it --device /dev/ttyACM0 \
  -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py -p /dev/ttyACM0 monitor

# Counter example: every experiment setting is in idf.py menuconfig
# (TinyBFT Configuration + Counter Example Configuration).  One image
# serves all boards (Role: Automatic), flashed to every port with:
#   ./flash_all.fish
```

**Role switching (counter):** `Counter Example Configuration > Role of this board`. The default, *Automatic*, compiles both roles into one image; at boot each board finds itself in the node list (MAC for ESP-NOW, IP for UDP) and runs as a replica if its entry is named `node*` (`Byz_node_is_replica()`), otherwise as the client. *Replica* / *Client* force a role (a board whose entry disagrees restarts with a message). See `examples/counter/README.md`. `espflash` alone does NOT flash the SPIFFS image; use `idf.py flash` or `flash_all.fish`.

**Kconfig changes:** a plain `idf.py build` does not re-run the configuration step when only this component's `Kconfig.projbuild` changed; run `idf.py reconfigure` (or `menuconfig`) first. Check Kconfig style with `python -m kconfcheck <file>` (ESP-IDF's checker, in the IDF Python environment).

### After making changes

Review build output before pushing. **First, attempt to verify using a local ESP-IDF installation.** If a local ESP-IDF toolchain is unavailable, fall back to the Docker environment.

**Local verification (preferred):**
```bash
# Verify using local toolchain
cd examples/counter
# Activate your local IDF environment (e.g. . $IDF_PATH/export.sh or fish get_idf)
idf.py build
```

**Docker verification (fallback):**
If Docker is unavailable locally, build errors will surface in CI.
```bash
# Full build check via Docker (counter example = most complex):
cd examples/counter && bash memcalc.sh   # verify tbft_replica_t size
docker run --rm -it -v $PWD:/esp-tinybft -w /esp-tinybft/examples/counter \
  -u $(id -u) -e HOME=/tmp espressif/idf:v6.0.1 \
  idf.py build
```

**Protocol changes: also run the host simulator.** `tests/sim` builds the component for Linux (shims for FreeRTOS, esp_timer, lwIP and NVS; no RSA) and runs 7 replicas + 1 client as processes over loopback UDP through boot races, staggered boot, crashes, reboots and packet loss. It then checks that all replicas hold the same state and that every request ran exactly once:

```bash
python3 tests/sim/run.py              # every scenario (about 10 minutes)
python3 tests/sim/run.py S1 S3 S6b    # selected ones
python3 tests/sim/run.py --san address,undefined S1 S4   # with sanitizers
```

It does not replace a run on boards: there is no radio and no real crypto timing (see `tests/sim/README.md`).

Runtime logging uses `ESP_LOGI/LOGE/LOGW` with per-file `TAG` constants, visible in `idf.py monitor`.

## Architecture

### Layer overview

```
include/esp-tinybft.h       ← public API (Byz_* functions, libbyz-compatible)
src/tbft_libbyz.c           ← API implementation + config file parser
src/tbft_replica.c/h        ← PBFT state machine (message dispatch + all handlers)
src/tbft_node.c/h           ← Transport delegation (send/recv → transport layer)
src/tbft_transport.h        ← Transport abstraction interface
src/tbft_transport_udp.c    ← UDP backend (lwIP sockets)
src/tbft_transport_espnow.c ← ESP-NOW backend (auto fragmentation/reassembly)
src/tbft_principal.c/h      ← per-peer crypto: HMAC-SHA256 (hot path) + RSA-2048
src/tbft_state.c/h          ← application state: CoW, partition tree, fetch
src/tbft_config.h           ← all compile-time constants (sourced from Kconfig)
src/tbft_types.h            ← typedefs: scalars, crypto structs, bitmap, network, state block
```

### Language standard: C23

The component and both examples are written in C23 (GNU dialect, `-std=gnu23`), the default C version of ESP-IDF ≥ 6.0 (`docs/en/api-guides/c.rst`; v5.5 was still `gnu17`), which `idf_component.yml` already requires. The `CMakeLists.txt` files pin it with `target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu23)`, the method that page documents, and `include/esp-tinybft.h` stops with `#error` on a pre-C23 C compiler (C++ callers are fine). The ESP toolchain (GCC 15.2) reports `__STDC_VERSION__ 202311L`.

Conventions:
- `nullptr` instead of `NULL`; `bool`/`true`/`false` are keywords (no `<stdbool.h>`); `static_assert` instead of `_Static_assert`.
- Typed `static constexpr` constants instead of object-like `#define`s, keeping the type the literal had (`int` for counts and timeouts, `size_t` for `sizeof` sums). Macros stay where the preprocessor needs them: `#if` tests, `#ifndef` override defaults, string literals spliced into format strings (`MACSTR`), the Kconfig bridge in `tbft_config.h`, and public-API macros (`BYZ_REQ_HEADER_SIZE`). File-scope `constexpr` objects need `static` (C gives `const` objects external linkage), and an unused one in a `.c` file is a `-Werror` failure (`-Wunused-const-variable`); mark per-configuration ones `[[maybe_unused]]`.
- C23 attribute syntax: `[[gnu::packed]]` on wire structs (same layout as `__attribute__((packed))`), `alignas(N)`, `[[maybe_unused]]` instead of `(void)x;`, `[[noreturn]]`, and `[[nodiscard]]` on the crypto/verify functions in `tbft_principal.h` and `tbft_node.h` (IDF builds with `-Werror`, so an ignored result fails the build).
- Log tags as `static constexpr char TAG[] = "...";` (an array in flash, not a pointer variable).
- `= {}` to zero-initialise new objects; keep `memset` for resetting existing ones (a compound-literal assignment of a large struct can put a big temporary on a FreeRTOS stack).
- `ckd_add` / `ckd_mul` from `<stdckdint.h>` for size arithmetic on untrusted or caller-supplied values. `<stdbit.h>` is not available in the toolchain's C library.

### Crypto migration (ESP-IDF v6.0 / MbedTLS 4.x)

MbedTLS 4.x removed several classic APIs. The component uses the **PSA Crypto API** for symmetric primitives while keeping `mbedtls/pk.h` for asymmetric operations:

| Primitive | Old API (removed) | Current API |
|-----------|-------------------|-------------|
| SHA-256 hash | `mbedtls_sha256()` | `psa_hash_compute(PSA_ALG_SHA_256, ...)` |
| HMAC-SHA256 | `mbedtls_md_hmac_*()` (private in 4.x) | `psa_import_key` + `psa_mac_compute(PSA_ALG_HMAC(PSA_ALG_SHA_256))` |
| RSA encrypt | `mbedtls_pk_encrypt()` | `mbedtls_pk_import_into_psa` + `psa_asymmetric_encrypt` |
| RSA decrypt | `mbedtls_pk_decrypt()` | `mbedtls_pk_import_into_psa` + `psa_asymmetric_decrypt` |
| RSA sign | `mbedtls_pk_sign()` (with rng) | `mbedtls_pk_sign()` (no rng param in 4.x) |
| RSA verify | `mbedtls_pk_verify()` | unchanged |

Removed headers: `mbedtls/sha256.h`, `mbedtls/entropy.h`, `mbedtls/ctr_drbg.h` — all moved to private/.

### Transport layer

The component supports two interchangeable transport backends, selected at compile time via `CONFIG_TBFT_TRANSPORT_TYPE` in `menuconfig`:

| Backend | Config value | Protocol | Max payload | Notes |
|---------|-------------|----------|-------------|-------|
| **UDP** | `TBFT_TRANSPORT_UDP` | lwIP sockets | Unbounded | Supports multicast |
| **ESP-NOW** | `TBFT_TRANSPORT_ESPNOW` | esp_now API | 1470 bytes (v2.0) | Auto fragmentation + reassembly |

The transport interface (`tbft_transport.h`) abstracts send/recv/peer-registration behind a uniform API. Both backends handle the same message formats and authentication — switching transports requires no changes to the BFT protocol logic.

#### ESP-NOW fragmentation

ESP-NOW v2.0 limits each packet to 1470 bytes. Messages exceeding this are automatically fragmented with a 4-byte header (`msg_id`, `frag_idx`, `frag_total`) and reassembled on the receiving side. Fragmentation works for both unicast and broadcast sends. The reassembly system uses 8 concurrent slots with 5-second timeout per message.

`espnow_recv_cb` runs in the **WiFi task context** (not an ISR); standard task-context FreeRTOS APIs (`xSemaphoreTake`, `xQueueSend`) are used throughout.

#### Config file formats

The config file has `num_nodes` entries which include both replicas and clients. Lines with hostname prefix `"node"` are counted as replicas; all others are clients. `TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS` (Kconfig, defaults 4+1) must be ≥ `num_nodes`.

**UDP mode** (per node line):
```
<hostname> <ip> <port> <pubkey_path>
```

**ESP-NOW mode** (per node line):
```
<hostname> <mac_address> <pubkey_path>
```
MAC format: `xx:xx:xx:xx:xx:xx` (e.g. `aa:bb:cc:dd:ee:ff`). The parser auto-detects the format by checking for `:` in the second field.

#### Host app responsibilities (ESP-NOW)

When using ESP-NOW, the host application must:
1. Initialize WiFi (`esp_wifi_init`, `esp_wifi_set_mode`, `esp_wifi_start`)
   - The interface is chosen by `CONFIG_TBFT_ESPNOW_IF_STA` (default) or `CONFIG_TBFT_ESPNOW_IF_AP`; **every board must use the same one**. STA: `esp_wifi_set_mode(WIFI_MODE_STA)`, never connect, set the channel after `esp_wifi_start()`. AP (legacy) uses the softAP MAC = base MAC + 1 and keeps the radio on with beacons.
2. Call `esp_now_init()` before `Byz_init_*()`. The ESP-NOW transport then registers its own `esp_now_recv_cb` and `esp_now_send_cb`
3. Peer MAC addresses are registered via `esp_now_add_peer` automatically (PHY rate from `TBFT_ESPNOW_RATE_*` via `esp_now_set_peer_rate_config()`)
4. Receive power saving (`TBFT_ESPNOW_POWER_*`, station only) is applied by the transport right after the callbacks: `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`, `esp_now_set_wake_window()`, `esp_wifi_connectionless_module_set_wake_interval()`. Retry timing follows from it (`TBFT_ESPNOW_RETRY_DELAY_MS` / `_SPAN_MS` in `tbft_config.h`: half window, interval + 20 ms). With it Off the app may set `WIFI_PS_NONE`

### TinyBFT static memory model

All memory of the component is static: it never calls `malloc`. The replica (or the client; a process runs one, so they share the storage), the transports with their queues and send task, the FreeRTOS objects (created with the `...Static` APIs) and the file buffer used at start-up are global objects, so the heap cannot fragment because of the component and the linker checks at build time that everything fits. ESP-IDF's own components (Wi-Fi, lwIP, mbedtls/PSA, esp_timer, NVS) still use the heap.

Three region structs inside `tbft_replica_t` hold all in-flight messages:

| Region | File | Contents |
|--------|------|----------|
| `tbft_agreement_region_t` | `tbft_agreement_region.*` | `tbft_agreement_slice_t[WINDOW_SIZE]`, each slice holds `tbft_prepared_cert_t` (Pre_prepare + Prepare) + `tbft_commit_cert_t` (Commit) |
| `tbft_checkpoint_region_t` | `tbft_checkpoint_region.*` | Checkpoint messages per (seqno, replica_id) in `slots[]`; a slot is tagged with its seqno and taken over by a newer checkpoint that maps to it |
| `tbft_special_region_t` | `tbft_special_region.*` | The newest View_change of each replica, the New_view that started the current view, the last signed Reply per client (uses `TBFT_SR_SLOT` macro with buf+len+valid) |

All three regions live directly inside `tbft_replica_t`. Indexing into `agreement_region` is `(seqno - head) & mask` — the window head advances on each stable checkpoint (`tbft_ar_truncate`).

`tbft_ar_truncate` does not erase the slots that fall out of the window. Each slot is tagged with its sequence number and is emptied only when a sequence number of the new window first uses it (`tbft_ar_slice`). Until then `tbft_ar_peek` still returns its messages, so they can be re-sent to a replica that is a little behind. Always check `tbft_ar_in_range` before `tbft_ar_slice`: calling it for an older seqno would take over a live slot.

### Certificate quorum logic

`tbft_certificate.c` uses a macro `CERT_IMPL(prefix, cert_t, msg_slot_size)` to generate identical quorum logic for Commit, Prepare, and Checkpoint certificates. Each certificate stores up to `f+1` (`TBFT_CERT_MAX_VALS`) distinct message values (beyond that a quorum is guaranteed) and tracks per-sender bits in a `tbft_bitmap_t` (uint64_t). The certificate becomes **complete** when any value accumulates `threshold` matches: 2f+1 for Commit and Checkpoint, 2f for Prepare. Prepares come from backups only (the primary's Pre_prepare stands for its Prepare, and a Prepare from the primary is ignored), so "prepared" means the Pre_prepare plus 2f backups, 2f+1 replicas in all. Counting a Prepare from the primary too would let a faulty primary get two correct replicas prepared on different requests for one seqno.

### Replica struct layout

`tbft_replica_t` (in `tbft_replica.h`) begins with `tbft_node_t node` as its first member, enabling safe pointer-aliasing between `tbft_replica_t *` and `tbft_node_t *`. Everything it needs is embedded by value: the three regions (`ar`, `cr`, `sr`), the state with its checkpoint copy and partition tree, the principals (`node.principal_store`) and the event group's storage. `tbft_libbyz.c` keeps it in a static union with the client's node and buffers (`s_mem`); the transports keep their state in static singletons (one per process).

Additional important fields:
- `tbft_view_info_t vi` — view-change protocol state
- `EventGroupHandle_t evt_group` — Event group used to decouple timer callbacks (which execute in the system timer task) from heavy operations like RSA cryptography. Callbacks are signal-only — `vtimer_cb` and `stimer_cb` call `xEventGroupSetBits` and return immediately; heavy work runs in the `tbft_replica_run()` loop.
- Four timers: `vtimer`, `stimer`, `rtimer`, `ntimer` (all `tbft_itimer_t` wrapping `esp_timer_handle_t`). Timers signal the event group rather than executing directly.
- Request queues: `rqueue`, `ro_rqueue` (circular FIFOs, `TBFT_RQUEUE_MAX` entries, default 8 via Kconfig)
- Buffers: `ndet_buf[TBFT_NDET_BUF_SIZE]`, `out_buf[TBFT_MAX_MESSAGE_SIZE]`
- `sys_state[TBFT_SYS_STATE_SIZE]` — the library's own replicated state: the client table (`tbft_client_entry_t` per client: last executed request id, its seqno and reply). Its blocks follow the application's and are checkpointed and transferred with them.
- `running` flag, `vtimer_period_us`, `stimer_period_us`
- Timers are initialised in `tbft_replica_init()` but **not started** — `Byz_init_replica()` sets the correct periods from the config file and starts them

### Message flow

1. `tbft_replica_run()` loops on `tbft_node_recv()` → dispatches by `hdr->tag` (1–17).
2. The client broadcasts its Request. Backups forward it once to the primary and start the view-change timer. The primary queues it; `tbft_replica_send_pre_prepare` gives a sequence number to every queued request the window has room for and broadcasts a Pre_prepare (request + non-det choices, digest over both, HMAC authenticator). A request that already ran or already has a sequence number is not ordered again.
3. Backups store the Pre_prepare and broadcast Prepare. The primary sends no Prepare.
4. Prepared (Pre_prepare + 2f matching Prepares from backups, own included) → broadcast Commit.
5. Committed (2f+1 matching Commits) → `tbft_replica_execute_committed` executes strictly in sequence-number order: it stops at the first seqno without its commit quorum or its Pre_prepare and never skips one. The result goes into the client table and, signed, to the client. Every `CHECKPOINT_INTERVAL` seqnos: `tbft_state_checkpoint` + broadcast Checkpoint.
6. 2f+1 matching Checkpoints make a checkpoint stable: `tbft_replica_mark_stable` moves the window (`ar`, `cr`) if the local state at that seqno has the same digest. A replica that has not executed that far first asks the others for the missing messages and fetches the state if that does not get it there.

### Protocol rules the handlers rely on

**Exactly-once execution.** A request whose id is not above the last one executed for its client (client table, replicated state) is never executed again. If it is that last one, the replica re-sends the reply; the signed message is cached in `sr.reply`, so this costs no RSA signature. This is what makes client retransmission safe.

**Client retransmission and request ids.** `Byz_recv_reply` re-sends the request to every replica after `TBFT_CLIENT_RETRANSMIT_MS`, then after twice as long, and so on until the reply timeout. A request id is `client id | boot epoch | counter`. The boot epoch is a counter kept in NVS (namespace `tbft`), so ids keep growing across client reboots; without it the replicas would ignore a rebooted client. The application must call `nvs_flash_init()` before `Byz_init_client()`.

**Retransmission between replicas (Status).** Status never changes the receiver's view or state (it has no authenticator); it only makes the receiver re-send:
- to a replica in an older view: the stored, signed New_view of the current view;
- to a replica that still lacks what the receiver had one Status period earlier, or whose Status has `TBFT_STATUS_WANT` (sent at once when execution is stuck on a missing message): the Checkpoint message of the receiver's stable checkpoint and, for each seqno it still holds, its Commit, the Pre_prepare byte for byte as the primary sent it (so the primary's MAC still verifies) and its Prepare.

A Pre_prepare whose MAC does not verify (relabelled by a New_view, or the primary's key changed) is accepted only when its digest is already fixed by the New_view or by 2f+1 Commits. Pre_prepares, Prepares and Commits of any view but the current one are ignored: a replica moves to a newer view only through its New_view.

**View change.** A replica sends View_change for the next view when its timer expires, and joins a view change once f+1 other replicas ask for a higher view, so one replica alone moves nobody. After sending View_change it sends no Prepare or Commit until a New_view arrives, but it still executes what the others commit. The timer doubles after each attempt that brought no progress. A replica whose view change has no support (f+1 peers still report normal work) keeps its target instead of moving further away. View_change carries the stable checkpoint (seqno and digest) and, for every seqno above it the replica saw prepared or committed, the view and digest. With 2f+1 of them the new primary sends New_view: the highest stable checkpoint (`min`, `min_digest`) and, for each seqno up to the highest one reported, the request prepared in the highest view, or a null request. `nv_install` (new primary and backups alike) keeps Pre_prepare bodies with the right digest, drops the Prepares and Commits of older views and prepares the re-proposed requests again; a replica behind `min` fetches that checkpoint. The new view starts with empty request queues and no pending request: requests that were not ordered come back with the client's retransmission (so a request whose client went away does not keep the view-change timer running).

**Late boot and reboot.** New_key lists which peer keys the sender holds (`have_keys`); a replica whose key is missing there answers with its own New_key, so a replica that boots late or reboots has its keys within about a second instead of waiting for the periodic broadcast. It then gets the current view (New_view), the stable checkpoint (Checkpoint messages, then a state fetch) and the messages of the window through Status.

**Known limits.** View_change entries carry no proof and New_view is not checked against the View_change messages, so a Byzantine replica can lie during a view change (crashes, restarts and message loss are handled). A replica that restarts has lost its protocol state; until it has caught up it counts toward f.

### Crypto paths

- **Hot path** (Pre_prepare, Prepare, Commit, Checkpoint): HMAC-SHA256 authenticator appended after the fixed-size rep struct. `tbft_node_gen_auth` fills one slot per remote replica.
- **Slow path** (Request, View_change, New_view): RSA-2048 signature via `tbft_principal_sign` / `tbft_principal_verify_sig`.
- Session key rotation: `tbft_principal_encrypt_new_key` / `tbft_principal_decrypt_new_key` — imports RSA key into PSA, uses `psa_asymmetric_encrypt/decrypt` with PKCS#1 v1.5 to distribute fresh HMAC session keys.

#### Anti-replay design (why timestamp checking is disabled)

`tbft_principal_verify_mac_in_with_replay_check` intentionally skips the `timestamp_us` monotonicity check (`(void)msg_time_us; return true;`). This is by design, not an oversight:

1. **Staggered boot problem**: ESP32 boards use `esp_timer_get_time()` which starts at 0 on each boot. When boards are flashed sequentially (minutes apart), their timers diverge — a freshly-booted board's timestamps appear "stale" (too far in the past) to boards that have been running longer. A 5-minute slack only helps in one direction; the lagging board's messages are always rejected.

2. **Key rotation as replay protection**: Each `New_key` exchange generates fresh random HMAC session keys (`esp_fill_random`). Once keys rotate, any captured pre-rotation message fails HMAC verification under the new key — the attacker can't replay across key boundaries.

3. **Sender bitmap as duplicate guard**: Each certificate (`tbft_certificate.c`) tracks per-sender contributions via `tbft_bitmap_t`. A replayed Prepare/Commit from the same sender for the same seqno is rejected as a duplicate sender. Cross-seqno replay fails because the HMAC covers the entire message including `seqno`.

Together, key rotation + sender bitmap provide equivalent protection without requiring synchronized clocks across embedded devices.

**Do not re-enable timestamp-based anti-replay** without solving the staggered-boot clock synchronization problem first.

### Configuration file format (section 12 of ARCHITECTURE.md)

Read by `parse_config()` in `tbft_libbyz.c`. Plain text, line-based:
```
<service_name>
<f>
<auth_timeout_ms>
<num_nodes>
<multicast_ip>
<hostname> <ip> <port> <pubkey_der_path>   # repeated num_nodes times
<view_change_timeout_ms>
<status_timeout_ms>
<recovery_timeout_ms>
```
Public keys are DER files on SPIFFS. Private key path is passed separately as `priv_config` to `Byz_init_replica` / `Byz_init_client`.

**Note:** In the embedded context, `Byz_init_replica` cannot determine the local node's IP, so it defaults to `local_id = 0` with a warning log. The config parser matches hostnames against the local IP list, but falls back to node 0.

## Examples

Two example applications live under `examples/`, each demonstrating a full replica + client setup.

### examples/simple_wallet

4 accounts, transfer between them. 4 nodes (3 replicas + 1 client, f=1).

### examples/counter

Single integer counter starting at 0, incremented by client requests. 8 nodes (7 replicas + 1 client, f=2).

### Example structure (both follow the same pattern)

```
examples/<name>/
├── CMakeLists.txt            # ESP-IDF project root
├── main/
│   ├── CMakeLists.txt        # registers main.c, embeds SPIFFS image
│   ├── idf_component.yml     # depends on esp-tinybft (override_path: "../../..")
│   ├── Kconfig.projbuild     # role (counter: Automatic/Replica/Client), WiFi SSID/PS mode
│   └── main.c                # app_state, exec_cb, wifi_init, replica_task, client_task
├── sdkconfig.defaults         # TinyBFT Kconfig overrides (transport, sizes, window)
├── flash_all.fish             # (counter only) flash build/ to every /dev/ttyACM* in parallel
├── partitions.csv             # custom partition table with SPIFFS
├── gen_configs.sh             # generates RSA keys + config_udp.txt + config_espnow.txt
├── spiffs_image/              # keys + config files — embedded into SPIFFS at build time
│   ├── config_udp.txt
│   └── config_espnow.txt
└── memcalc.sh                 # (counter only) computes tbft_replica_t size from config
```

### Adding a new example

1. Copy an existing example structure.
2. Define the app state array, request/response structs, and `exec_cb`.
3. Run `./gen_configs.sh` to generate RSA keys and config files.
4. Edit `sdkconfig.defaults` with appropriate `TBFT_MAX_NUM_REPLICAS`, `TBFT_WINDOW_SIZE`, etc.
5. The `memcalc.sh` script computes the component's static memory before building; `idf.py size` reports what the build really uses. Allocate nothing from the heap in the example either: use the `...Static` FreeRTOS APIs as the examples do.

### Memory sizing for ESP32-C3

Nothing is allocated at run time: the component's memory is static and the linker fails the build if it does not fit. `tbft_replica_t` is driven primarily by:

| Region | Key scaling factors |
|--------|---------------------|
| `ar` (agreement) | `WINDOW_SIZE × MAX_MESSAGE_SIZE × CERT_MAX_VALS` |
| `cr` (checkpoint) | `NUM_CKPT_SLOTS × MAX_NUM_REPLICAS × CKPT_MSG_SIZE` |
| `sr` (special) | `MAX_NUM_REPLICAS × (352 + 48 × WINDOW_SIZE)` (view_change) + `MAX_NUM_CLIENTS × (MAX_REPLY_SIZE + 316)` (reply cache) |
| `rqueues` (×2) | `2 × RQUEUE_MAX × MAX_MESSAGE_SIZE` |
| client table | `MAX_NUM_CLIENTS × (24 + MAX_REPLY_SIZE)`, rounded up to whole blocks |

Also inside it: the checkpoint copy (`TBFT_MAX_STATE_BLOCKS × TBFT_BLOCK_SIZE`), the partition tree (every level full for `TBFT_MAX_STATE_BLOCKS` leaves) and the principals (`(MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) × sizeof(tbft_principal_t)`, 1,712 bytes each with ESP-IDF 6.1). Outside it: the ESP-NOW transport (8 reassembly slots, 16 + 8 queue entries of `MAX_MESSAGE_SIZE`, an 8 KB send task stack) or the 280-byte UDP transport, and a ~2 KB file buffer. On the original ESP32 static DRAM is limited to 160 KB (ESP-IDF *Memory Types* guide); the ESP32-C3 has no such limit.

**New_key constrains `MAX_MESSAGE_SIZE`**: the message carries `(n-1)` RSA-encrypted HMAC keys plus an RSA signature:

```
New_key_bytes = sizeof(tbft_new_key_rep_t)               // 32
              + (n - 1) × sizeof(tbft_new_key_slot_t)    // (n-1)×260
              + TBFT_SIG_SIZE                            // 256
```

With n=7: `32 + 6×260 + 256 = 1848` — `out_buf` (sized to `TBFT_MAX_MESSAGE_SIZE`) must be ≥ this.

**Typical ESP32-C3 budget** (321,296 bytes of DRAM for static data; the rest is heap for ESP-IDF): with 7 replicas keep `WINDOW_SIZE=4` and `MAX_MESSAGE_SIZE=2048`. With the counter example's settings `tbft_replica_t` is 79,072 bytes and the whole ESP-NOW build uses 272,092 bytes of static DRAM (49 KB left as heap for Wi-Fi, lwIP and mbedtls), the UDP build 191,140. Run `memcalc.sh` to estimate (its `tbft_replica_t` total matches `sizeof` for that configuration) and `idf.py size` to check.

`tbft_config.h` defines all constants from `CONFIG_TBFT_*` Kconfig values and enforces these `_Static_assert` checks:

- `TBFT_BLOCK_SIZE` must be a power of 2.
- `TBFT_WINDOW_SIZE` must be a power of 2 **and** > `TBFT_CHECKPOINT_INTERVAL`.
- `TBFT_MAX_NUM_REPLICAS >= 4`.
- `TBFT_MAX_NUM_REPLICAS <= 64` (bitmap is uint64_t).
- `TBFT_MAX_REPLY_SIZE < TBFT_MAX_MESSAGE_SIZE`.
- `TBFT_MAX_MESSAGE_SIZE % 8 == 0`.
- `TBFT_P_LEVELS >= 2`.
- A View_change and a New_view covering `TBFT_WINDOW_SIZE` sequence numbers must fit in `TBFT_MAX_MESSAGE_SIZE` (`tbft_special_region.h`).
- `2 <= TBFT_MAX_STATE_BLOCKS <= TBFT_P_CHILDREN³` (static partition tree).

Derived constants: `TBFT_DIGEST_SIZE` (32), `TBFT_HMAC_SIZE` (32), `TBFT_SIG_SIZE` (256), `TBFT_AUTH_SIZE`, `TBFT_NUM_CKPT_SLOTS`, `TBFT_MAX_FAULTY`, `TBFT_CERT_MAX_VALS` (f+1), `TBFT_P_CHILDREN`.

Additional Kconfig-sourced constants (with `#ifndef` guards for override):
- `TBFT_MAX_STATE_BLOCKS` (default 16) — max state blocks (application + client table); sizes the static checkpoint copy, digests, fetch queue and partition tree
- `TBFT_RQUEUE_MAX` (default 8) — request queue depth
- `TBFT_NDET_BUF_SIZE` (default 256) — non-det choices buffer
- `TBFT_P_LEVELS` (default 4) — Merkle partition tree depth
- `TBFT_ANTI_REPLAY_WINDOW_US` (default 30s, from `TBFT_ANTI_REPLAY_WINDOW_MS * 1000`)

- `MAX_NUM_REPLICAS` drives static buffer sizing across all three regions — keep it as small as the deployment allows.
- Default `MAX_NUM_REPLICAS=4` targets a single-fault-tolerant cluster (f=1, n=4).

### Input validation

Every message handler performs bounds and identity checks before touching protocol state:

- `handle_request`: `command_size` validated non-negative and no integer overflow (`command_size <= TBFT_MAX_MESSAGE_SIZE - sizeof(rep) - sizeof(sig)`); `cid` must be a client id (`num_replicas <= cid < num_principals`)
- `handle_prepare`, `handle_commit`, `handle_checkpoint`, `handle_view_change`, `handle_fetch`: sender id validated `>= 0 && < num_replicas`
- `handle_meta_data`: `n_parts` validated non-negative and `<= TBFT_P_CHILDREN` before the size arithmetic `sizeof(rep) + n_parts * sizeof(tbft_part_info_t) <= len`
- `handle_view_change`: `body_size >= sizeof(tbft_view_change_rep_t)` and `body_size <= len` before reading the RSA signature; at most one checkpoint entry and `TBFT_WINDOW_SIZE` request entries, all inside the body
- `handle_new_view`: signature of the new view's primary; `n_prep == max - min`, at most `TBFT_WINDOW_SIZE` entries, one per seqno `min+1 .. max` in order
- `send_pre_prepare`: `aligned_needed <= sizeof(r->out_buf)` overflow guard before writing the Pre-prepare message to `out_buf`

### State transfer (fetch protocol)

When a replica detects it has fallen behind, `tbft_state_start_fetch` sets `in_fetch=true` and enqueues a root-level Fetch. The fetching replica walks the Merkle partition tree (`tbft_ptree_t`) top-down, comparing local digests (`block_digests[]`) to received `Meta_data` messages and requesting only mismatched subtrees, until it reaches leaf-level `Data` messages that overwrite individual `tbft_block_t` entries.

State uses a dirty bitmap (`cowb[]` — array of uint64_t, not a single bitmap) to track blocks modified since the last checkpoint. `block_digests[]` holds per-block SHA-256 digests. Checkpoint records (seqno + root digest) are stored in `ckpt_records[]` (max `TBFT_NUM_CKPT_SLOTS`). `last_stable` tracks the highest stable checkpoint seqno.

`tbft_state_checkpoint` also copies every block into `image`. A replica answers Fetch only for the checkpoint its image holds (`tbft_state_can_serve`), and from the image, so the fetcher gets exactly the state that checkpoint's digest covers even while the replier keeps executing. The fetcher knows the certified root digest (from 2f+1 Checkpoint messages or from the New_view) and adopts the fetched state only if its root digest matches (`finish_fetch`); otherwise it fetches again from the next replica. After a timeout it aims at the newest certified checkpoint, or asks another replica.
