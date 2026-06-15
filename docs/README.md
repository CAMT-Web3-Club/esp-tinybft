<!-- docs/README.md — index for the topic-focused documentation. -->

# esp-tinybft Documentation

`esp-tinybft` is a pure-C implementation of PBFT (Practical Byzantine Fault
Tolerance) tuned for ESP32-class MCUs running ESP-IDF v6.0+. It compiles
as a managed component, ships with two interchangeable transport backends
(UDP over lwIP, ESP-NOW with auto-fragmentation) and a full PSA Crypto
migration (MbedTLS 4.x), and reserves all in-flight protocol state in
statically-allocated regions inside `tbft_replica_t` — no `malloc` in the
steady-state hot path. The default deployment is `f = 1` (n = 4 replicas)
or `f = 2` (n = 7); the verified target is the 4 MB / ~80 KB heap ESP32-C3.

This folder is the project's topic-focused reference set. Each file
documents one slice of the system; this README is the index that ties
them together.

## Documentation map

| File | One-line description |
|------|----------------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Deep-reference: BFT model, layer diagram, protocol phases, fault-tolerance matrix, full system architecture diagram. |
| [01-build-and-toolchain.md](01-build-and-toolchain.md) | Local + Docker toolchain, `idf.py` commands, role switching, partition table, CI workflow, `.clang-tidy`, common build errors. |
| [02-configuration.md](02-configuration.md) | Kconfig options, derived constants, `_Static_assert` invariants, SPIFFS config file format (UDP + ESP-NOW), private key loading, parser edge cases. |
| [03-architecture-overview.md](03-architecture-overview.md) | Layer diagram, public API surface, per-module responsibilities, `tbft_replica_t` layout, lifecycle (init -> run -> shutdown), replica sequence diagram. |
| [04-transport-layer.md](04-transport-layer.md) | `tbft_transport_t` interface, UDP and ESP-NOW backends, multicast / broadcast, send/recv paths, ESP-NOW fragmentation & reassembly, AP-vs-base MAC handling, threading model. |
| [05-crypto-subsystem.md](05-crypto-subsystem.md) | PSA migration table, `tbft_principal_t` struct, HMAC and RSA paths, New_key rotation, `tbft_auth_t` authenticator layout, MbedTLS 4.x API differences. |
| [06-replica-state-machine.md](06-replica-state-machine.md) | `tbft_replica_run` main loop, dispatch table, per-handler walkthroughs, event-group decoupling, sequence-number monotonicity, send paths, `execute_committed` and `mark_stable` flows. |
| [07-static-memory-model.md](07-static-memory-model.md) | Why static allocation, the three regions (`ar`, `cr`, `sr`), indexing math, request queues, ndet/out buffers, full replica struct field table, memory math. |
| [08-message-protocol.md](08-message-protocol.md) | Common header, all 18 message tags, per-message `__attribute__((packed))` structs, wire format alignment, authentication appendices, `out_buf` sizing. |
| [09-certificates-and-quorum.md](09-certificates-and-quorum.md) | `CERT_IMPL` X-macro pattern, generated cert struct layout, `tbft_bitmap_t` duplicate guards, threshold formulas, `tbft_prepared_cert_t` semantics, memory cost per cert. |
| [10-view-changes.md](10-view-changes.md) | When view-changes fire, `tbft_view_info_t`, primary assignment, send/handle/construction flows, New_view installation, RSA signature rules, suppression guards, sequence diagrams. |
| [11-state-transfer.md](11-state-transfer.md) | `tbft_state_start_fetch` entry, Merkle partition tree, Fetch / Meta_data / Data flow, CoW bitmap, `block_digests[]`, `ckpt_records[]`, v0.7.2 4×WINDOW fix. |
| [12-checkpointing.md](12-checkpointing.md) | Checkpoint interval, `send_checkpoint` and `handle_checkpoint`, stable-checkpoint detection (2f+1), `tbft_replica_mark_stable`, `tbft_ar_truncate`, `tbft_cr_truncate`, above-window slots. |
| [13-anti-replay.md](13-anti-replay.md) | Why timestamp-based anti-replay is disabled, staggered-boot problem, replacement mechanisms (HMAC key rotation + per-sender bitmap), "do not re-enable" warning. |
| [14-timers-and-tasks.md](14-timers-and-tasks.md) | `tbft_itimer_t` (esp_timer wrapper), four conceptual timer slots, event-group decoupling, run-loop processing, timer period config, why no heavy work in callbacks. |
| [15-input-validation.md](15-input-validation.md) | Layered validation strategy, per-handler bounds and identity checks, integer-overflow guards, why these checks matter on no-MMU ESP32-C3. |
| [16-memory-sizing.md](16-memory-sizing.md) | Size drivers, worked examples for (f=1, n=4) and (f=2, n=7), `New_key` size derivation, `out_buf` sizing, `memcalc.sh` algorithm, hardware verification steps. |
| [17-examples.md](17-examples.md) | `counter` (7 replicas + 1 client) and `simple_wallet` (3 replicas + 1 client) walkthroughs, `gen_configs.sh`, app state, request/response structs, `exec_cb` algorithms, recipe for a third example. |
| [18-development.md](18-development.md) | Code style (`.clang-tidy`), logging convention, debug recipes, v0.7.1 RSA-keyparse regression, v0.7.2 4×WINDOW regression, commit conventions, hardware test procedure. |
| [19-api-reference.md](19-api-reference.md) | Full `Byz_*` public API: buffer types, callbacks, client API, replica API, module-level singletons, reply collection, lifecycle snippet. |

## Reading order

### 1. Newcomer — get something building, then understand the flow

1. [01-build-and-toolchain.md](01-build-and-toolchain.md) — install or
   start the Docker toolchain, build `examples/counter`, and flash a
   board. Skim the partition table, role switching, and `.clang-tidy`
   sections.
2. [03-architecture-overview.md](03-architecture-overview.md) — read the
   layer diagram and the per-module summary; understand that
   `tbft_replica_t` is one big struct with three embedded regions.
3. [19-api-reference.md](19-api-reference.md) — skim the buffer types,
   callbacks, and lifecycle snippet; this is the only surface the host
   app touches.
4. [06-replica-state-machine.md](06-replica-state-machine.md) — follow
   the `tbft_replica_run` main loop and the dispatch table to see how
   Request becomes Pre_prepare becomes Prepare becomes Commit becomes
   `exec_cb` and Reply.
5. [17-examples.md](17-examples.md) — open `examples/counter` /
   `examples/simple_wallet` and trace the `exec_cb` against the message
   flow you just read.

### 2. Contributor — change something without breaking the budget

1. [03-architecture-overview.md](03-architecture-overview.md) — confirm
   which module owns the symbol you want to change.
2. [07-static-memory-model.md](07-static-memory-model.md) — internalise
   the three-region model and the indexing math; remember that the
   steady-state path performs no heap allocation.
3. [02-configuration.md](02-configuration.md) — every constant you want
   to raise is a Kconfig value, and every Kconfig value feeds a
   `_Static_assert` in `tbft_config.h`.
4. [16-memory-sizing.md](16-memory-sizing.md) — before raising
   `MAX_NUM_REPLICAS`, `WINDOW_SIZE`, or `MAX_MESSAGE_SIZE`, run
   `examples/counter/memcalc.sh` and confirm the replica still fits in
   the ESP32-C3's ~80 KB free heap.
5. [11-state-transfer.md](11-state-transfer.md) and
   [12-checkpointing.md](12-checkpointing.md) — if you are touching
   the recovery path, read these for the CoW, Merkle, and truncation
   invariants.
6. [18-development.md](18-development.md) — code style, log convention,
   commit message format, and the v0.7.x regression write-ups that
   encode hard-won lessons.

### 3. Auditor / security reviewer — verify the threat model

1. [05-crypto-subsystem.md](05-crypto-subsystem.md) — PSA migration
   table, hot path (HMAC-SHA256) vs slow path (RSA-2048), New_key
   rotation, `tbft_auth_t` layout, MbedTLS 4.x caveats.
2. [13-anti-replay.md](13-anti-replay.md) — the timestamp check is
   intentionally a no-op. Read the staggered-boot problem and the
   replacement mechanisms (key rotation + per-sender bitmap) before
   flagging this as a vulnerability.
3. [15-input-validation.md](15-input-validation.md) — per-handler
   bounds and identity checks, integer-overflow guards,
   `_Static_assert` invariants; this is the threat surface for
   malformed messages.
4. [10-view-changes.md](10-view-changes.md) — RSA signature rules on
   `View_change` and `New_view`, the suppression guards that prevent
   the cluster from splintering into divergent views.
5. [02-configuration.md](02-configuration.md) — the SPIFFS config file
   format and parser edge cases; a misconfigured file should fail
   `Byz_init_replica` rather than silently truncate the cluster.
6. [ARCHITECTURE.md](ARCHITECTURE.md) — full system architecture and
   the BFT fault-tolerance matrix.

## Key concepts

| Concept | Definition |
|---------|-----------|
| **PBFT** | Castro-Liskov PBFT. Cluster of `n = 3f + 1` replicas tolerates up to `f` Byzantine faults via a three-phase commit (Pre_prepare, Prepare, Commit) plus view-changes. |
| **Hot path** | Pre_prepare, Prepare, Commit, Checkpoint. Authenticated with HMAC-SHA256 (`tbft_auth_t` array of `n - 1` slots, one per remote replica). |
| **Slow path** | Request, Reply, View_change, New_view, New_key. Authenticated with RSA-2048 PKCS#1 v1.5 signatures via PSA Crypto. |
| **Static memory model** | All in-flight protocol messages live in three embedded regions — `ar` (agreement), `cr` (checkpoint), `sr` (special) — inside `tbft_replica_t`. No `malloc` on the hot path; the size is known at link time. |
| **Agreement region (`ar`)** | Circular buffer of `WINDOW_SIZE` slices, each holding one `tbft_prepared_cert_t` (Pre_prepare + Prepare cert) and one `tbft_commit_cert_t`. Indexed by `(seqno - head) & mask`. |
| **Checkpoint region (`cr`)** | `NUM_CKPT_SLOTS` in-window slots + `MAX_NUM_REPLICAS` above-window slots. Each slot tracks up to `f + 1` candidate digests so a Byzantine replica cannot poison the winning digest by arriving first. |
| **Special region (`sr`)** | Slow-path storage: `View_change[n]`, `View_change_ack[n²]`, `New_view`, `New_key`, `Request[MAX_NUM_CLIENTS]`, `Reply[MAX_NUM_CLIENTS]`. |
| **Quorum certificate** | Per-seqno object that groups matching messages by digest and signals "complete" when one digest accumulates `2f + 1` (Commit, Checkpoint) or `2f` (Prepare) contributions. Generated by a single `CERT_IMPL` X-macro. |
| **Bitmap (`tbft_bitmap_t`)** | `uint64_t` that acts as a per-sender duplicate guard inside every certificate. Set bit `i` means replica `i` has already contributed. |
| **New_key rotation** | Periodically the primary encrypts a fresh 32-byte HMAC session key under each peer's RSA public key (PKCS#1 v1.5) and broadcasts the result. Recipients install the new key as their in-key for the sender. |
| **Anti-replay (deliberately off)** | Timestamp-based monotonicity check is disabled because `esp_timer_get_time()` resets to 0 on every boot. Key rotation + per-sender bitmap provide equivalent protection. |
| **Fetch protocol** | A lagging replica walks the Merkle partition tree: send `Fetch` for `(level, index)`, receive `Meta_data` listing child digests, descend into mismatched subtrees, terminate at leaf-level `Data` messages that overwrite state blocks. |
| **Checkpoint stability** | A checkpoint at seqno `s` is stable when `2f + 1` Checkpoint messages with matching state digests have been collected; the replica then advances `last_stable` and truncates `ar` and `cr`. |
| **Primary = v mod n** | Single source of truth for "who is the primary" — `tbft_node_primary(view)`. Replicas never re-derive this from raw view numbers. |
| **Event-group decoupling** | Timer callbacks (`vtimer_cb`, `stimer_cb`) only call `xEventGroupSetBits`. Heavy work (RSA signing, fetch re-pathing) runs in the `tbft_replica_run` loop, never in the timer task. |
| **PSA migration** | ESP-IDF v6.0 ships MbedTLS 4.x, which removed the classic SHA-256 and HMAC APIs. The component uses `psa_hash_compute` / `psa_mac_compute` / `psa_mac_verify` and the mbedtls `pk` parser as the SPKI / PKCS#8 -> PSA bridge. |

## Cross-reference matrix

| Topic | Primary doc(s) | Secondary doc(s) |
|-------|----------------|------------------|
| Build, Docker, role switching | [01-build-and-toolchain.md](01-build-and-toolchain.md) | [18-development.md](18-development.md), [17-examples.md](17-examples.md) |
| Kconfig & SPIFFS config | [02-configuration.md](02-configuration.md) | [01-build-and-toolchain.md](01-build-and-toolchain.md), [16-memory-sizing.md](16-memory-sizing.md) |
| Public API (`Byz_*`) | [19-api-reference.md](19-api-reference.md) | [03-architecture-overview.md](03-architecture-overview.md), [17-examples.md](17-examples.md) |
| Architecture / layering | [03-architecture-overview.md](03-architecture-overview.md) | [ARCHITECTURE.md](ARCHITECTURE.md), [07-static-memory-model.md](07-static-memory-model.md) |
| Transport (UDP / ESP-NOW) | [04-transport-layer.md](04-transport-layer.md) | [01-build-and-toolchain.md](01-build-and-toolchain.md), [02-configuration.md](02-configuration.md) |
| Crypto (HMAC, RSA, New_key) | [05-crypto-subsystem.md](05-crypto-subsystem.md) | [13-anti-replay.md](13-anti-replay.md), [10-view-changes.md](10-view-changes.md) |
| Replica state machine / dispatch | [06-replica-state-machine.md](06-replica-state-machine.md) | [08-message-protocol.md](08-message-protocol.md), [15-input-validation.md](15-input-validation.md) |
| Static memory regions | [07-static-memory-model.md](07-static-memory-model.md) | [16-memory-sizing.md](16-memory-sizing.md), [03-architecture-overview.md](03-architecture-overview.md) |
| Wire format / message structs | [08-message-protocol.md](08-message-protocol.md) | [06-replica-state-machine.md](06-replica-state-machine.md) |
| Quorum certificates | [09-certificates-and-quorum.md](09-certificates-and-quorum.md) | [07-static-memory-model.md](07-static-memory-model.md), [08-message-protocol.md](08-message-protocol.md) |
| View changes | [10-view-changes.md](10-view-changes.md) | [05-crypto-subsystem.md](05-crypto-subsystem.md), [06-replica-state-machine.md](06-replica-state-machine.md) |
| State transfer / fetch | [11-state-transfer.md](11-state-transfer.md) | [12-checkpointing.md](12-checkpointing.md), [18-development.md](18-development.md) |
| Checkpointing | [12-checkpointing.md](12-checkpointing.md) | [11-state-transfer.md](11-state-transfer.md), [09-certificates-and-quorum.md](09-certificates-and-quorum.md) |
| Anti-replay | [13-anti-replay.md](13-anti-replay.md) | [05-crypto-subsystem.md](05-crypto-subsystem.md) |
| Timers & tasks | [14-timers-and-tasks.md](14-timers-and-tasks.md) | [06-replica-state-machine.md](06-replica-state-machine.md) |
| Input validation | [15-input-validation.md](15-input-validation.md) | [02-configuration.md](02-configuration.md), [06-replica-state-machine.md](06-replica-state-machine.md) |
| Memory sizing | [16-memory-sizing.md](16-memory-sizing.md) | [07-static-memory-model.md](07-static-memory-model.md), [17-examples.md](17-examples.md) |
| Examples | [17-examples.md](17-examples.md) | [19-api-reference.md](19-api-reference.md), [16-memory-sizing.md](16-memory-sizing.md) |
| Development / debug / regressions | [18-development.md](18-development.md) | [14-timers-and-tasks.md](14-timers-and-tasks.md) |

## Source map

| Source file | Documenting doc(s) |
|-------------|--------------------|
| `include/esp-tinybft.h` | [19-api-reference.md](19-api-reference.md), [03-architecture-overview.md](03-architecture-overview.md) |
| `src/tbft_libbyz.c` | [03-architecture-overview.md](03-architecture-overview.md), [19-api-reference.md](19-api-reference.md), [02-configuration.md](02-configuration.md) |
| `src/tbft_replica.c` / `tbft_replica.h` | [06-replica-state-machine.md](06-replica-state-machine.md), [07-static-memory-model.md](07-static-memory-model.md), [10-view-changes.md](10-view-changes.md), [15-input-validation.md](15-input-validation.md) |
| `src/tbft_node.c` / `tbft_node.h` | [03-architecture-overview.md](03-architecture-overview.md), [07-static-memory-model.md](07-static-memory-model.md) |
| `src/tbft_principal.c` / `tbft_principal.h` | [05-crypto-subsystem.md](05-crypto-subsystem.md), [13-anti-replay.md](13-anti-replay.md) |
| `src/tbft_agreement_region.c` / `.h` | [07-static-memory-model.md](07-static-memory-model.md), [12-checkpointing.md](12-checkpointing.md) |
| `src/tbft_checkpoint_region.c` / `.h` | [07-static-memory-model.md](07-static-memory-model.md), [12-checkpointing.md](12-checkpointing.md) |
| `src/tbft_special_region.c` / `.h` | [07-static-memory-model.md](07-static-memory-model.md) |
| `src/tbft_view_info.c` / `.h` | [10-view-changes.md](10-view-changes.md) |
| `src/tbft_state.c` / `.h` | [11-state-transfer.md](11-state-transfer.md), [12-checkpointing.md](12-checkpointing.md) |
| `src/tbft_certificate.c` / `.h` | [09-certificates-and-quorum.md](09-certificates-and-quorum.md) |
| `src/tbft_prepared_cert.c` / `.h` | [09-certificates-and-quorum.md](09-certificates-and-quorum.md) |
| `src/tbft_message.c` / `.h` | [08-message-protocol.md](08-message-protocol.md) |
| `src/tbft_transport.h` | [04-transport-layer.md](04-transport-layer.md) |
| `src/tbft_transport_udp.c` | [04-transport-layer.md](04-transport-layer.md) |
| `src/tbft_transport_espnow.c` | [04-transport-layer.md](04-transport-layer.md) |
| `src/tbft_partition.c` / `.h` | [11-state-transfer.md](11-state-transfer.md) |
| `src/tbft_itimer.c` / `.h` | [14-timers-and-tasks.md](14-timers-and-tasks.md) |
| `src/tbft_config.h` | [02-configuration.md](02-configuration.md), [16-memory-sizing.md](16-memory-sizing.md) |
| `src/tbft_log.h` | [09-certificates-and-quorum.md](09-certificates-and-quorum.md) |
| `src/tbft_types.h` | [05-crypto-subsystem.md](05-crypto-subsystem.md), [09-certificates-and-quorum.md](09-certificates-and-quorum.md) |
| `examples/counter/` | [17-examples.md](17-examples.md), [16-memory-sizing.md](16-memory-sizing.md) |
| `examples/simple_wallet/` | [17-examples.md](17-examples.md) |
| `Kconfig.projbuild` | [02-configuration.md](02-configuration.md) |
| `.clang-tidy` | [18-development.md](18-development.md), [01-build-and-toolchain.md](01-build-and-toolchain.md) |
| `idf_component.yml` | [01-build-and-toolchain.md](01-build-and-toolchain.md), [18-development.md](18-development.md) |
| `AGENT.md` | [01-build-and-toolchain.md](01-build-and-toolchain.md) |

## Examples map

| Example | Path | Replicas | f | App state | Doc |
|---------|------|---------:|---|-----------|-----|
| **counter** | `examples/counter/` | 7 | 2 | Single `int32_t` counter incremented by client requests | [17-examples.md § Counter example](17-examples.md#counter-example) |
| **simple_wallet** | `examples/simple_wallet/` | 3 | 1 | 4 int32 account balances with transfer semantics | [17-examples.md § Simple_wallet example](17-examples.md#simple_wallet-example) |

Both examples share the same project skeleton (ESP-IDF root, `main/`
component, `gen_configs.sh` for RSA keys + config files, SPIFFS image
embed, custom partition table). The recipe for adding a third example
lives in [17-examples.md § How to add a new example](17-examples.md#how-to-add-a-new-example).

## How to update these docs

**Source of truth.** The C source in `src/` and the public header in
`include/esp-tinybft.h` are the canonical source of truth. Every doc
file is downstream of those. When the two disagree, the source wins and
the doc is wrong; fix the doc, not the code.

**File:line citation convention.** Every claim in these docs that
points at a specific symbol, function, struct field, or compile-time
constant carries a `path:line` reference (e.g. `src/tbft_replica.c:395`).
When you change the source, `git grep` the file:line citation to find
the paragraphs that need updating. The most stable citations are the
struct field declarations in headers (they survive function-body
refactors); the most fragile are line numbers inside function bodies.

**Topic files (01-19).** Each numbered file owns exactly one slice of
the system. When adding new functionality, extend the existing topic
file (or split a sub-section if it grows past a few screens) rather
than creating a new top-level doc. The Cross-reference matrix and
Source map above list which source files each doc owns.

**ARCHITECTURE.md vs the topic files.** `ARCHITECTURE.md` is the
deep-reference: the BFT model, the full layer diagram, the
fault-tolerance matrix, and the protocol phase table. The numbered
topic files are the working reference: each one deepens a single
slice. Cross-link rather than duplicate.

**v0.7.x changelog tracking.** Component versions live in
`idf_component.yml`, and every release commit uses the
`v0.X.Y[-tagline]` subject. The two most recent regressions are
written up in [18-development.md](18-development.md): the
`v0.7.1-rsa-keyparse-fix` (PSA migration) and the
`v0.7.2-state-transfer-fix` (4×WINDOW fetch cap) write-ups encode
hard-won lessons. When a new regression is fixed, follow the same
template (Symptom / Root cause / Fix / Lesson) and link the commit hash.

**Last-verified line.** Every doc file ends with a
`Last verified against: <commit> (<tagline>)` line. When you update a
doc, bump that line to the current HEAD. The footer of this README
carries the same hash; keep them in sync.

**Footer of every doc.** The closing line of each topic file reads:

```text
Last verified against: <commit> (<tagline>).
```

This is the contract: if the commit hash in the footer is older than
HEAD, the doc has stale content and should be re-checked.

---

*Last verified against `c937fbe` (v0.7.2-state-transfer-fix: remove 4×WINDOW limit on state fetch) on branch `revert-to-v0.7.0`.*
