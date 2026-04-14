# AGENT.md

This file provides guidance to AI agents (Claude Code, Gemini CLI, etc.) when working with code in this repository.

## Build commands

This is an **ESP-IDF component** (not a standalone app). It is built as part of a host project that adds it as a managed component or local component.

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

There is no dedicated test target or test suite yet. Runtime logging uses `ESP_LOGI/LOGE/LOGW` with per-file `TAG` constants, visible in `idf.py monitor`.

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

ESP-NOW v2.0 limits each packet to 1470 bytes. Messages exceeding this are automatically fragmented with a 4-byte header (`msg_id`, `frag_idx`, `frag_total`) and reassembled on the receiving side. Fragmentation works for both unicast and broadcast sends. The reassembly system uses 4 concurrent slots with 5-second timeout per message.

`espnow_recv_cb` runs in the **WiFi task context** (not an ISR); standard task-context FreeRTOS APIs (`xSemaphoreTake`, `xQueueSend`) are used throughout.

#### Config file formats

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
2. The ESP-NOW transport registers its own `esp_now_recv_cb` and `esp_now_send_cb`
3. Peer MAC addresses are registered via `esp_now_add_peer` automatically

### TinyBFT static memory model

The key design decision: instead of `malloc`/`free` for every protocol message, three **statically-allocated region structs** hold all in-flight messages:

| Region | File | Contents |
|--------|------|----------|
| `tbft_agreement_region_t` | `tbft_agreement_region.*` | `tbft_agreement_slice_t[WINDOW_SIZE]`, each slice holds `tbft_prepared_cert_t` (Pre_prepare + Prepare) + `tbft_commit_cert_t` (Commit) |
| `tbft_checkpoint_region_t` | `tbft_checkpoint_region.*` | Checkpoint messages per (seqno, replica_id) in `slots[]`, plus `above_window[]` for out-of-window entries |
| `tbft_special_region_t` | `tbft_special_region.*` | View_change × n, View_change_ack × n², New_view, New_key, Request/Reply per client (uses `TBFT_SR_SLOT` macro with buf+len+valid) |

All three regions live directly inside `tbft_replica_t`. Indexing into `agreement_region` is `(seqno - head) & mask` — the window head advances on each stable checkpoint (`tbft_ar_truncate`).

### Certificate quorum logic

`tbft_certificate.c` uses a macro `CERT_IMPL(prefix, cert_t, msg_slot_size)` to generate identical quorum logic for Commit, Prepare, and Checkpoint certificates. Each certificate stores up to `f+1` (`TBFT_CERT_MAX_VALS`) distinct message values (beyond that a quorum is guaranteed) and tracks per-sender bits in a `tbft_bitmap_t` (uint64_t). The certificate becomes **complete** when any value accumulates `threshold` (2f+1) matches.

### Replica struct layout

`tbft_replica_t` (in `tbft_replica.h`) begins with `tbft_node_t node` as its first member, enabling safe pointer-aliasing between `tbft_replica_t *` and `tbft_node_t *`. The three static regions (`ar`, `cr`, `sr`) are embedded by value — there is no heap allocation in the steady-state protocol path.

Additional important fields:
- `tbft_view_info_t vi` — view-change protocol state
- `EventGroupHandle_t evt_group` — Event group used to decouple timer callbacks (which execute in the system timer task) from heavy operations like RSA cryptography. Callbacks are signal-only — `vtimer_cb` and `stimer_cb` call `xEventGroupSetBits` and return immediately; heavy work runs in the `tbft_replica_run()` loop.
- Four timers: `vtimer`, `stimer`, `rtimer`, `ntimer` (all `tbft_itimer_t` wrapping `esp_timer_handle_t`). Timers signal the event group rather than executing directly.
- Request queues: `rqueue`, `ro_rqueue` (circular FIFOs, `TBFT_RQUEUE_MAX` entries, default 16 via Kconfig)
- Buffers: `ndet_buf[TBFT_NDET_BUF_SIZE]`, `out_buf[TBFT_MAX_MESSAGE_SIZE]`
- `running` flag, `vtimer_period_us`, `stimer_period_us`
- Timers are initialised in `tbft_replica_init()` but **not started** — `Byz_init_replica()` sets the correct periods from the config file and starts them

### Message flow

1. `tbft_replica_run()` loops on `tbft_node_recv()` → dispatches by `hdr->tag` (1–17).
2. Primary receives a Request → `rqueue_push` → `tbft_replica_send_pre_prepare` builds and broadcasts Pre_prepare with embedded request set + non-det choices + HMAC authenticator.
3. Backups receive Pre_prepare → store in `ar` → broadcast Prepare.
4. All receive Prepare → `tbft_ar_add_prepare` → when complete, broadcast Commit.
5. All receive Commit → `tbft_ar_add_commit` → when complete, `tbft_replica_execute_committed` calls `exec_cb`, sends Reply, and checkpoints every `CHECKPOINT_INTERVAL` seqnos.
6. On checkpoint: `tbft_state_checkpoint` + broadcast Checkpoint → `tbft_cr_store` → when stable (`2f+1` matches), `tbft_replica_mark_stable` truncates static regions (`ar`, `cr`).

### Crypto paths

- **Hot path** (Pre_prepare, Prepare, Commit, Checkpoint): HMAC-SHA256 authenticator appended after the fixed-size rep struct. `tbft_node_gen_auth` fills one slot per remote replica.
- **Slow path** (Request, View_change, New_view): RSA-2048 signature via `tbft_principal_sign` / `tbft_principal_verify_sig`.
- Session key rotation: `tbft_principal_encrypt_new_key` / `tbft_principal_decrypt_new_key` — imports RSA key into PSA, uses `psa_asymmetric_encrypt/decrypt` with PKCS#1 v1.5 to distribute fresh HMAC session keys.

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

### Key Kconfig constraints

`tbft_config.h` defines all constants from `CONFIG_TBFT_*` Kconfig values and enforces these `_Static_assert` checks:

- `TBFT_BLOCK_SIZE` must be a power of 2.
- `TBFT_WINDOW_SIZE` must be a power of 2 **and** > `TBFT_CHECKPOINT_INTERVAL`.
- `TBFT_MAX_NUM_REPLICAS >= 4`.
- `TBFT_MAX_REPLY_SIZE < TBFT_MAX_MESSAGE_SIZE`.
- `TBFT_P_LEVELS >= 2`.

Derived constants: `TBFT_DIGEST_SIZE` (32), `TBFT_HMAC_SIZE` (32), `TBFT_SIG_SIZE` (256), `TBFT_AUTH_SIZE`, `TBFT_NUM_CKPT_SLOTS`, `TBFT_MAX_FAULTY`, `TBFT_CERT_MAX_VALS` (f+1), `TBFT_P_CHILDREN`.

Additional Kconfig-sourced constants (with `#ifndef` guards for override):
- `TBFT_MAX_STATE_BLOCKS` (default 256) — max state blocks
- `TBFT_RQUEUE_MAX` (default 16) — request queue depth
- `TBFT_NDET_BUF_SIZE` (default 256) — non-det choices buffer
- `TBFT_P_LEVELS` (default 4) — Merkle partition tree depth
- `TBFT_ANTI_REPLAY_WINDOW_US` (default 30s, from `TBFT_ANTI_REPLAY_WINDOW_MS * 1000`)

- `MAX_NUM_REPLICAS` drives static buffer sizing across all three regions — keep it as small as the deployment allows.
- Default `MAX_NUM_REPLICAS=4` targets a single-fault-tolerant cluster (f=1, n=4).

### Input validation

Every message handler performs bounds and identity checks before touching protocol state:

- `handle_request`: `command_size` validated non-negative and no integer overflow (`command_size <= TBFT_MAX_MESSAGE_SIZE - sizeof(rep) - sizeof(sig)`); `cid >= 0 && cid < num_principals`
- `handle_prepare`, `handle_commit`, `handle_checkpoint`, `handle_view_change`, `handle_fetch`: sender id validated `>= 0 && < num_replicas`
- `handle_meta_data`: `n_parts` validated non-negative and `<= TBFT_P_CHILDREN` before the size arithmetic `sizeof(rep) + n_parts * sizeof(tbft_part_info_t) <= len`
- `handle_view_change`: `body_size >= sizeof(tbft_view_change_rep_t)` and `body_size <= len` before reading the RSA signature
- `send_pre_prepare`: `aligned_needed <= sizeof(r->out_buf)` overflow guard before writing the Pre-prepare message to `out_buf`

### State transfer (fetch protocol)

When a replica detects it has fallen behind, `tbft_state_start_fetch` sets `in_fetch=true` and enqueues a root-level Fetch. The fetching replica walks the Merkle partition tree (`tbft_ptree_t`) top-down, comparing local digests (`block_digests[]`) to received `Meta_data` messages and requesting only mismatched subtrees, until it reaches leaf-level `Data` messages that overwrite individual `tbft_block_t` entries.

State uses a CoW bitmap (`cowb[]` — array of uint64_t, not a single bitmap) to track dirty blocks. `block_digests[]` holds per-block SHA-256 digests. Checkpoint records are stored in `ckpt_records[]` (max `TBFT_NUM_CKPT_SLOTS`). `last_stable` tracks the highest stable checkpoint seqno.
