# esp-tinybft

A pure-C implementation of [PBFT](https://pmg.csail.mit.edu/papers/osdi99.pdf) (Practical Byzantine Fault Tolerance) for ESP-IDF, optimised for embedded systems. It uses static memory allocation throughout — no `malloc` in the hot path — and exposes a `libbyz`-compatible API.

## Features

- **PBFT consensus** — tolerates up to `f` Byzantine faults in a cluster of `n = 3f + 1` replicas
- **Static memory model** — three pre-allocated region structs (agreement, checkpoint, special); no heap allocation in the steady-state protocol path
- **Dual transport** — UDP over lwIP sockets or ESP-NOW with automatic fragmentation/reassembly; switchable via `menuconfig`
- **PSA Crypto / MbedTLS 4.x** — HMAC-SHA256 (hot path) via PSA; RSA-2048 sign/verify via `mbedtls/pk.h`
- **libbyz-compatible API** — drop-in for projects that already use the original libbyz interface
- **FreeRTOS-native** — runs in a dedicated task; timer callbacks are signal-only (event group); heavy protocol work dispatched in the run loop

## Requirements

- ESP-IDF v6.0 or later (MbedTLS 4.x / PSA Crypto)
- Supported targets: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2

## Quick Start

### 1. Add the component

Add `esp-tinybft` to your host project's `idf_component.yml` (create it in your project root or `main/` if it does not exist):

```yaml
dependencies:
  phukrit7171/esp-tinybft: ">=0.2.2"
```

Then let the IDF Component Manager fetch it:

```bash
idf.py update-dependencies
```

The component is downloaded from the [Espressif Component Registry](https://components.espressif.com/components/phukrit7171/esp-tinybft) and cached under `managed_components/`.

### 2. Configure

```bash
idf.py menuconfig
# Navigate to: Component config → TinyBFT Configuration
```

Key options:

| Option | Default | Description |
|---|---|---|
| `TBFT_TRANSPORT_TYPE` | UDP | Transport backend: UDP or ESP-NOW |
| `TBFT_MAX_NUM_REPLICAS` | 4 | Cluster size (n = 3f+1, default f=1) |
| `TBFT_MAX_MESSAGE_SIZE` | 1440 | Maximum protocol message size (bytes) |
| `TBFT_WINDOW_SIZE` | 16 | Sequence number window |
| `TBFT_CHECKPOINT_INTERVAL` | 8 | Checkpoint every N requests |
| `TBFT_RQUEUE_MAX` | 8 | Max queued requests (memory: 2 × N × msg_size) |
| `TBFT_NDET_BUF_SIZE` | 256 | Non-deterministic choices buffer |
| `TBFT_MAX_STATE_BLOCKS` | 256 | Max state blocks for CoW / partition tree |
| `TBFT_P_LEVELS` | 4 | Merkle partition tree depth |
| `TBFT_ANTI_REPLAY_WINDOW_MS` | 30000 | Anti-replay window (ms, 0 = disabled) |

> **For ESP32-C3 constrained deployments**, `TBFT_MAX_MESSAGE_SIZE=1024`, `TBFT_WINDOW_SIZE=8`, `TBFT_CHECKPOINT_INTERVAL=4`, `TBFT_RQUEUE_MAX=4` are recommended (saves ~60KB RAM). See `examples/simple_wallet/sdkconfig.defaults`.

### 3. Build and flash

```bash
idf.py build
idf.py flash monitor
```

## Usage

### Replica

```c
#include "esp-tinybft.h"

static uint8_t app_state[4096];

int exec_cb(Byz_req *in, Byz_rep *out, Byz_buffer *ndet, int cid, bool ro) {
    // Read in->contents / in->size, write reply to out->contents / out->size
    return 0;
}

void replica_task(void *arg) {
    Byz_init_replica("config.txt", "priv.der",
                     app_state, sizeof(app_state),
                     exec_cb, NULL, 0, NULL, 0);
    Byz_replica_run();  // blocks indefinitely
}
```

> **Important:** Call `Byz_modify(ptr, size)` or `Byz_modify1(ptr)` before writing to any byte of `app_state`. This triggers the copy-on-write snapshot used for state transfer and rollback.

### Client

```c
#include "esp-tinybft.h"

void client_task(void *arg) {
    Byz_init_client("config.txt", "priv.der", 0);

    Byz_req req;
    Byz_rep rep;
    Byz_alloc_request(&req, 64);
    memcpy(req.contents, my_cmd, 64);
    req.size = 64;

    Byz_invoke(&req, &rep, false);  // send + wait for f+1 matching replies

    // use rep.contents[0..rep.size)
    Byz_free_reply(&rep);
    Byz_free_request(&req);
}
```

## Configuration Files

### UDP mode

```
<service_name>
<f>
<auth_timeout_ms>
<num_nodes>
<multicast_ip>
<hostname> <ip> <port> <pubkey_der_path>   # one line per node
...
<view_change_timeout_ms>
<status_timeout_ms>
<recovery_timeout_ms>
```

### ESP-NOW mode

Same format but node lines use MAC addresses instead of IP/port:

```
<hostname> <mac_address> <pubkey_der_path>   # MAC: aa:bb:cc:dd:ee:ff
```

The parser auto-detects the format per line by checking for `:` in the second field.

Public keys are DER files (RSA-2048) stored on SPIFFS. Pass the private key path as `priv_config` to `Byz_init_replica` / `Byz_init_client`.

## Transport Backends

| Backend | Config | Protocol | Max payload | Notes |
|---|---|---|---|---|
| UDP | `TBFT_TRANSPORT_UDP` | lwIP sockets | Unbounded | Supports multicast |
| ESP-NOW | `TBFT_TRANSPORT_ESPNOW` | esp_now API | 1470 bytes | Auto fragmentation/reassembly |

### Input Validation

The replica enforces the following bounds checks before touching any protocol state:

- **Request handler**: `command_size` is validated non-negative and within message bounds; `cid` must be `>= 0 && < num_principals`
- **Prepare / Commit / Checkpoint / Fetch / View-change handlers**: sender id must be `>= 0 && < num_replicas`
- **Meta-data handler**: `n_parts` validated non-negative and `<= TBFT_P_CHILDREN` before performing the size arithmetic that walks the parts array
- **View-change handler**: `body_size` validated `>= sizeof(tbft_view_change_rep_t)` and `<= len` before reading the appended RSA signature
- **send_pre_prepare**: `aligned_needed` overflow guard ensures the assembled Pre-prepare fits in `out_buf` before any writes

### ESP-NOW host app requirements

When using the ESP-NOW backend, the host application must initialise WiFi **and ESP-NOW** before calling `Byz_init_replica`:

```c
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
esp_now_init();
// then call Byz_init_replica(...)
```

ESP-NOW data must be transmitted only after Wi-Fi is started, so the recommended order is: start Wi-Fi, call `esp_now_init()`, then initialise TinyBFT. The transport layer handles peer registration (`esp_now_add_peer`) automatically.

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full documentation covering:

- Layer overview and component diagram
- Static memory regions (agreement, checkpoint, special)
- Certificate quorum logic
- All 17 PBFT message types
- Protocol flow diagrams (Normal case, Checkpoint, View-change)
- State management and Merkle partition tree
- Crypto paths (PSA hot path / RSA slow path)
- Timer system and Kconfig reference

## Fault Tolerance

| `TBFT_MAX_NUM_REPLICAS` | f (faulty) | Quorum (2f+1) |
|---|---|---|
| 4 (default) | 1 | 3 |
| 7 | 2 | 5 |
| 10 | 3 | 7 |

## Changelog

### v0.3.0 — Security audit fixes

- **Fix #1 (Critical) — OOB bounds check in `handle_new_view`:** Added proof-size validation before reading `n_prep` entries. A malicious primary with `hdr.size == sizeof(*nv)` but `n_prep == WINDOW_SIZE` would cause out-of-bounds reads in the proof loops. Now `body_size >= sizeof(*nv) + n_prep * sizeof(tbft_vc_req_info_t)` is enforced.
- **Fix #2 (High) — Exponential backoff shift always computed as 0:** At three sites (`handle_view_change`, `handle_new_view`, `handle_status`), `r->node.view` was overwritten before computing the view-gap for the vtimer backoff period. The gap was always 0, clamping the shift to 1 and preventing exponential growth. Now captures `old_view` before the overwrite.
- **Fix #3 (High) — Per-proof verification in `tbft_vi_verify_nv`:** The New_view message's `n_prep` proofs were accepted without verification against collected View_change messages. A Byzantine primary could inject arbitrary proofs to advance `last_executed`. Now each proof must be attested by at least `f+1` collected VCs.
- **Fix #4 (High) — `last_stable` set without checkpoint quorum:** The window-head advance path in `handle_new_view` set `r->last_stable = r->seqno - 1` without any checkpoint quorum proof. Now only the window head advances; `last_stable` stays at the verified `nv->min`.
- **Fix #5 (Medium) — ESP-NOW shutdown semaphore race:** The `espnow_send_cb` could attempt `xSemaphoreGive` on a deleted semaphore during shutdown. Added `shutting_down` flag check before the semaphore operation.
- **Fix #6/#7 (Medium) — vtimer backoff wiped on progress:** Marking a stable checkpoint or sending a pre-prepare reset the vtimer to the base period, discarding any accumulated exponential backoff from prior view-change turbulence. Now uses half the current backoff period as the floor (`max(base, period/2)`).
- **Fix #8 (Low) — `fill_sent_us` not cleared on truncate:** `tbft_ar_truncate` cleared `commit_sent_us` but not `fill_sent_us`. When a slice was reused after window wrap, a stale `fill_sent_us` could bypass the fill throttle. Now both fields are cleared.
- **Fix #9 (Low) — Stale-view fill requests:** `handle_fill_request` now drops requests whose `fr->view != r->node.view`, avoiding wasted HMAC verification and PP re-transmission for old-view fills.
- **Fix #11 (Low) — Misleading comment:** Corrected a comment claiming fill responses could come from "any replica" — the HMAC keys restrict it to the primary.

### v0.2.19

- **Revert v0.2.18 primary-vtimer-suppress:** Preventing the primary from calling `send_view_change` on its own vtimer (v0.2.18) proved to be a regression — the primary must participate in the view-change protocol to contribute its prepared certificates. Without the primary's view-change message the new primary may lack complete prepare state, causing cascading incomplete view changes.
- **Skip enqueue during view-change instead:** The correct fix for the request-queue-full problem is to skip enqueuing client requests on the primary while `vi.in_progress` is true.  `send_pre_prepare` returns immediately in that state (line 2124), so any queued request would just sit, fill the queue, and get dropped with log spam.  Silently returning lets the client retry with exponential backoff; by the next attempt the view change will normally be complete and the correct primary handles the request.

### v0.2.17

- **Commit re-broadcast during fill gap:** When the fill mechanism detects a stuck seqno where the pre-prepare is stored and prepare quorum is reached but commit quorum is not, it now **actively re-broadcasts the local commit** every 500ms (Layer 1.5). Previously the replica waited passively for commits to arrive during the 10-second fill window; with exactly 2f+1 live replicas, a single commit packet dropped by an unreliable transport (ESP-NOW `no send credit`, UDP loss) could block consensus until the Layer-2 abandon fired. Each re-send may trigger `handle_commit` on other replicas, which re-send their own commits — closing the gap through a chain reaction. Rate-limited to 500ms and gated on `tbft_ar_prepared` + `!tbft_ar_committed` (same guards as the existing `handle_commit` re-send path).

### v0.2.16

- **Prevent infinite state-fetch loop after node reboot:** Added `st->last_stable > r->last_stable` guard to the `handle_status` Path B fetch trigger (which already checked `st->last_executed > r->last_executed` and execution gap > `CHECKPOINT_INTERVAL`). After a rebooted node marks a stable checkpoint, Path B would previously re-fetch the same `last_stable` seqno whenever a peer had higher `last_executed` — a no-op that never advances the node. The guard aligns Path B with Path A (view catch-up), which already had this check.
- **Diagnostic logging for post-fetch digest mismatch:** Enhanced the `mark_stable` log message when a checkpoint digest mismatch is detected after a completed state fetch. The log now prints the first 8 bytes + last byte of both the winning checkpoint digest and the local ptree root digest, plus the `in_fetch` flag and `fetch_seqno`. This provides evidence to determine whether the mismatch is caused by stale checkpoint data in the CR (fixable by clearing CR) or a genuine state reconstruction bug in the ptree.

### v0.2.15

- **Fill short-circuit when PP is already stored:** When the fill mechanism's `pending_fill_seqno` slot already contains a pre-prepare, the Layer-1 fill request is now skipped — the gap is about missing commit certificates, not the PP itself. Previously the fill block re-requested the same PP every 500ms for 10 seconds, hitting `tbft_prepared_cert_add_pp`'s "slot occupied" check and wasting bandwidth, before the abandon log message misleadingly said "no replica had the PP" when the local replica had stored it on the first reception. The backup now waits passively for the missing commits; the 10-second abandon still fires as a last-resort fallback.
- **More accurate abandon log message:** When the fill mechanism's 10s abandon fires, the log now distinguishes between "PP stored but commit quorum not reached" (the most common case in production, likely network drop or Byzantine withholding) and "no replica had the PP" (the original gap-stall case from v0.2.14). The text shown on the operator's serial console is now actionable.
- **No protocol changes:** Wire format and message tags unchanged. This is a behaviour refinement to the v0.2.14 fill/abandon mechanism.

### v0.2.14

- **Fill timeout + abandon (recovery from PP loss after node failure):** When a stuck seqno cannot be filled by the current primary within 10 seconds (e.g., the original sender crashed before the broadcast reached any of the new primaries after view changes), the local replica now **abandons the seqno** — `last_executed` is force-advanced past it, unblocking the cluster. The client's request at that seqno is effectively a no-op on this replica; the client detects `TBFT_CLIENT_REPLY_TIMEOUT_MS` and must retransmit. Bounded state divergence (≤ `f+1` replicas) is recovered on the next checkpoint via the existing state-fetch protocol.

  A cross-view broadcast-fill (sending fill requests to all replicas) was considered but does not help: the PP stored in any backup's ar slot is for the original view, and the requester (in a later view) rejects PPs from earlier views in `handle_pre_prepare`. Abandon is the correct recovery for the cross-view PP-loss case.

  Fixes the "1 node down → no consensus" deadlock observed when a primary crashes mid-broadcast and no replica (including subsequent primaries) holds the pre-prepare.

- **`Byz_invoke_with_retry()` client API:** New library-level retry helper. Bounded attempts with exponential backoff (1s, 2s, 4s, ..., capped at 10s). Returns the number of attempts used on success. Both example apps (`counter`, `simple_wallet`) updated to use it with 3-attempt retry. Survives requests that are abandoned by the cluster due to the fill mechanism timing out.

- **No protocol changes:** Wire format and message tags unchanged. The escalation policy is purely a recovery-path improvement; consensus rules and message semantics are identical to v0.2.13.

### v0.2.13

- **Gap-fill (`Fill_request`):** Replicas can request a re-sent pre-prepare from the primary when commits arrive out of order. Closes the gap-stall bug where `seqno N+1` would commit before `N`, causing the execution loop to freeze for minutes until a view-change. The new `TBFT_MSG_FILL_REQUEST` (tag 18) is a 72-byte HMAC-authenticated unicast; the primary re-sends the original stored pre-prepare bytes (still HMAC-valid), letting the backup run the normal prepare/commit flow and close the gap. No protocol correctness regression.

### v0.2.0

All core PBFT protocol bugs fixed — system now reaches consensus end-to-end:

| Fix | Description |
|-----|-------------|
| Anti-replay slack | Increased from 60s to 300s to tolerate staggered boot times |
| hdr.size before HMAC | Set `hdr.size` BEFORE computing HMAC for Prepare, Commit, and Checkpoint messages |
| View advancement | Advance local view on receiving a higher-view pre-prepare |
| Embedded request padding | Allow 7-byte alignment padding in embedded request validation |
| Auth slot index | `tbft_node_auth_slot_index` uses receiver's `node_id`, not sender's |
| Primary `last_prepared` | Primary updates `last_prepared` when creating a pre-prepare |
| Checkpoint MAC verify | Receiver passes body size (not `hdr->size`) to `tbft_node_verify_auth` |
| Backup vtimer | Removed vtimer restart on every pre-prepare — prevents spurious view-changes |
| View-change cascade | Removed catch-up view-change in `handle_view_change` — prevents cascade to V+1 |

### v0.1.0

Initial release — PBFT consensus with static memory, dual transport (UDP/ESP-NOW), and libbyz-compatible API.

## License

- `LICENSE.tinybft` — BSD 3-Clause (TinyBFT modifications, Copyright 2022 Harald Böhm)
- `LICENSE.pbft` — MIT (original PBFT/libbyz, Copyright 1999–2001 Miguel Castro et al.)
