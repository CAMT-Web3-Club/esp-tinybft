# esp-tinybft

A pure-C implementation of [PBFT](https://pmg.csail.mit.edu/papers/osdi99.pdf) (Practical Byzantine Fault Tolerance) for ESP-IDF, optimised for embedded systems. All of its memory is static (it never calls `malloc`) and it exposes a `libbyz`-compatible API.

## Features

- **PBFT consensus** — tolerates up to `f` Byzantine faults in a cluster of `n = 3f + 1` replicas
- **Static memory only** — the component never calls `malloc`: replica, client, transports, FreeRTOS objects and buffers are static, so it cannot fragment the heap and the linker checks the RAM budget at build time (ESP-IDF's own components such as Wi-Fi, lwIP and mbedtls still use the heap)
- **Dual transport** — UDP over lwIP sockets or ESP-NOW with automatic fragmentation/reassembly; switchable via `menuconfig`
- **PSA Crypto / MbedTLS 4.x** — HMAC-SHA256 (hot path) via PSA; RSA-2048 sign/verify via `mbedtls/pk.h`
- **libbyz-compatible API** — drop-in for projects that already use the original libbyz interface
- **FreeRTOS-native** — runs in a dedicated task; timer callbacks are signal-only (event group); heavy protocol work dispatched in the run loop

## Requirements

- ESP-IDF v6.0 or later (MbedTLS 4.x / PSA Crypto)
- C23: the sources are written in GNU C23 (`-std=gnu23`), the default C
  version of ESP-IDF v6.0 and later. C files that include `esp-tinybft.h` must
  be compiled as C23 (the IDF default) or as C++.
- Supported targets: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2
- Enough static RAM: the replica's memory is reserved at link time
  (`examples/counter/memcalc.sh` estimates it, `idf.py size` reports it).
  The component and both examples are tuned for the ESP32-C3, whose whole
  DRAM can hold static data. The original ESP32 allows at most 160 KB of
  static DRAM (ESP-IDF *Memory Types* guide), less than the examples'
  settings need.
- An `nvs` partition, initialised with `nvs_flash_init()` before
  `Byz_init_client()`: the client keeps a boot counter there so that its
  request ids keep growing across resets (replicas never run a request id
  twice, so a client whose ids started over would be ignored).

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
| `TBFT_MAX_STATE_BLOCKS` | 16 | Max state blocks (application state + client table); sets the static checkpoint copy, digests and partition tree |
| `TBFT_P_LEVELS` | 4 | Merkle partition tree depth |
| `TBFT_ANTI_REPLAY_WINDOW_MS` | 30000 | Anti-replay window (ms, 0 = disabled) |
| `TBFT_CLIENT_REPLY_TIMEOUT_MS` | 10000 | How long `Byz_invoke` waits for f+1 matching replies |
| `TBFT_CLIENT_RETRANSMIT_MS` | 5000 | The client re-sends a request without f+1 replies after this time, then after twice as long, … (0 = never) |

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

> **Important:** Call `Byz_modify(ptr, size)` or `Byz_modify1(ptr)` when writing to any byte of `app_state`. It marks the blocks as modified, so the next checkpoint digests them; replicas compare these digests and transfer state by them.

### Client

```c
#include "esp-tinybft.h"

void client_task(void *arg) {
    // nvs_flash_init() must have run (app_main), see Requirements
    Byz_init_client("config.txt", "priv.der", 0);

    Byz_req req;
    Byz_rep rep;
    Byz_alloc_request(&req, 64);
    memcpy(req.contents, my_cmd, 64);
    req.size = 64;

    // send, re-send while needed, wait for f+1 matching replies; a request
    // is executed at most once even when it is re-sent
    Byz_invoke(&req, &rep, false);

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

- **Request handler**: `command_size` is validated non-negative and within message bounds; `cid` must be a client id (`>= num_replicas && < num_principals`)
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

What is handled (checked with the host simulator in `tests/sim`, see `AGENT.md`): replicas and the client booting at different times, crash of the primary (also of two primaries in a row), a replica or the client restarting, and lost messages. Every request runs at most once, in the same order on every replica, and lost messages are re-sent on request rather than on a timer.

Known limits: messages exchanged during a view change carry no proofs, so a *Byzantine* replica can misreport then (a faulty new primary is trusted); a replica that restarts has lost its protocol state and counts toward f until it has caught up.

## Changelog

### v0.2.9 (branch `fix/develop/v0.2.9`)

PBFT correctness and staggered boot:

| Fix | Description |
|-----|-------------|
| In-order execution | A replica never skips a sequence number: it waits for the commit quorum and the pre-prepare, asks for missing messages, or fetches the state of a certified checkpoint |
| Exactly-once | A replicated client table (last executed request id + reply per client) makes re-sent requests safe; the cached signed reply is re-sent |
| Client retransmission | `Byz_invoke` re-sends a request without f+1 replies (`TBFT_CLIENT_RETRANSMIT_MS`); request ids continue across client resets (boot epoch in NVS) |
| View change | Joined only when f+1 replicas ask; New_view re-proposes prepared requests (null requests for gaps) and is accepted from the view's primary without a local quorum, so late or restarted replicas can follow; Status no longer changes the view |
| Prepare quorum | Prepares come from backups only (the primary's pre-prepare stands for its prepare) |
| Retransmission | Status-driven: a replica re-sends the New_view, its Checkpoint and its Commit/Pre-prepare/Prepare messages to a peer that lacks them; the 500 ms commit re-send is gone |
| State transfer | Served from a copy of the checkpointed state and checked against the certified digest before it is used |
| Key exchange | New_key says which keys the sender holds; a replica whose key is missing answers at once, so a late or restarted replica is ready in about a second |
| Memory | No heap in the component: everything static, checked by the linker (counter example on ESP32-C3: `tbft_replica_t` 79,072 bytes including the checkpoint copy, partition tree and principals; ESP-NOW transport 75,160). Unused special-region and checkpoint-region slots removed (they took 30,000 bytes) |

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
