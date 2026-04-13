# esp-tinybft

A pure-C implementation of [PBFT](https://pmg.csail.mit.edu/papers/osdi99.pdf) (Practical Byzantine Fault Tolerance) for ESP-IDF, optimised for embedded systems. It uses static memory allocation throughout — no `malloc` in the hot path — and exposes a `libbyz`-compatible API.

## Features

- **PBFT consensus** — tolerates up to `f` Byzantine faults in a cluster of `n = 3f + 1` replicas
- **Static memory model** — three pre-allocated region structs (agreement, checkpoint, special); no heap allocation in the steady-state protocol path
- **Dual transport** — UDP over lwIP sockets or ESP-NOW with automatic fragmentation/reassembly; switchable via `menuconfig`
- **PSA Crypto / MbedTLS 4.x** — HMAC-SHA256 (hot path) via PSA; RSA-2048 sign/verify via `mbedtls/pk.h`
- **libbyz-compatible API** — drop-in for projects that already use the original libbyz interface
- **FreeRTOS-native** — runs in a dedicated task; timers use `esp_timer`

## Requirements

- ESP-IDF v6.0 or later (MbedTLS 4.x / PSA Crypto)
- Target: ESP32 family (tested on ESP32-S3)

## Quick Start

### 1. Add the component

Copy or symlink this directory into your host project's `components/` folder, or use the IDF Component Manager:

```bash
# From your host project root
mkdir -p components
ln -s /path/to/esp-tinybft components/esp-tinybft
```

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
| `TBFT_MAX_MESSAGE_SIZE` | 8192 | Maximum protocol message size |
| `TBFT_WINDOW_SIZE` | 256 | Sequence number window |
| `TBFT_CHECKPOINT_INTERVAL` | 128 | Checkpoint every N requests |

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

### ESP-NOW host app requirements

When using the ESP-NOW backend, the host application must initialise WiFi before calling `Byz_init_replica`:

```c
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
// then call Byz_init_replica(...)
```

The transport layer handles peer registration (`esp_now_add_peer`) automatically.

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

## License

- `LICENSE.tinybft` — BSD 3-Clause (TinyBFT modifications, Copyright 2022 Harald Böhm)
- `LICENSE.pbft` — MIT (original PBFT/libbyz, Copyright 1999–2001 Miguel Castro et al.)
