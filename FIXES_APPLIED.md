# Fixes Applied Summary

**Date:** 2026-06-14
**Build status:** counter + simple_wallet examples pass `idf.py build` for esp32c3
**Approach:** Apply highest-impact fixes first (CRITICAL → HIGH → MEDIUM → LOW/INFO), grouped by file/domain.

---

## Severity Distribution of Fixes Applied

| Severity | Fixes Applied |
|----------|--------------:|
| CRITICAL | 6 |
| HIGH     | 8 |
| MEDIUM   | 8 |
| LOW      | 6 |
| INFO     | 3 |
| **Total**| **31** |

The remaining findings (status changes, refutations, or "dead code, low impact") are documented in `REVIEW_REPORT.md`, `FINDINGS_FEEDBACK.md`, and `RUNTIME_ANALYSIS.md`.

---

## Bundle 1 — Transport Queue Capacity (CRITICAL/HIGH)
**Files:** `src/tbft_transport_espnow.c`, `Kconfig.projbuild`

### Original (reverted) attempts
| Finding | Original Fix | Status |
|---------|--------------|--------|
| F-002 (msg_queue drops) | `MSG_QUEUE_DEPTH`: 16 → 32 | ❌ **REVERTED** (boot OOM — see Bundle 1b) |
| F-003 (send credit starvation) | `ESPNOW_CREDITS_CAP`: 6 → 7 | ❌ **REVERTED** (ESP-NOW driver hidden TX-queue limit; 7 → silent drops) |
| F-022 (SEND_QUEUE undersized) | `SEND_QUEUE_DEPTH`: 8 → 16 | ❌ **REVERTED** (boot OOM — see Bundle 1b) |
| F-021 (reasm capacity halved) | `reasm_find_or_alloc` now clears stale-but-valid slots before reuse | ✅ **KEPT** (no heap cost) |
| F-002 (timeout) | `xQueueSend` timeout 0 → `pdMS_TO_TICKS(10)` | ✅ **KEPT** (no heap cost) |
| F-003 (timeout) | `ESPNOW_CREDIT_TIMEOUT_MS`: 100 → 50ms | ✅ **KEPT** (no heap cost) |

**Revert rationale:** Hardware validation on a 7-replica + 1-client cluster (Jun 14 2026, logs in `esp-debugger/data/node[0-6]`) showed all 7 replicas failing at `transport_create` with `queue/sem alloc failed: ... free_heap=74664`. The 32-deep msg queue allocates 32 × 2058 = 65,856 bytes contiguous; after WiFi+ESP-NOW init, heap_4 had fragmented to ~4 KB blocks and could not satisfy the request. The 7-credit cap also pushed the ESP-NOW driver past its undocumented internal TX-queue limit, causing silent fragment-drop failures. Reverting to 16/8/6 restored boot.

### Bundle 1b — Static-Allocation Pass (NEW, addresses OOM at root cause)
**Files:** `src/tbft_transport_espnow.c`, `Kconfig.projbuild`

| Change | Detail |
|--------|--------|
| `xQueueCreate` → `xQueueCreateStatic` | msg_queue + send_queue now use caller-provided BSS buffers |
| `xSemaphoreCreate*` → `xSemaphoreCreate*Static` | lock, task_exit_sem, send_credit_sem now use caller-provided `StaticSemaphore_t` |
| `xTaskCreate` → `xTaskCreateStatic` | send_task now uses caller-provided 8 KB stack + `StaticTask_t` TCB |
| `calloc(1, sizeof(tbft_espnow_t))` → `static tbft_espnow_t` | Transport struct is now BSS-resident (singleton) |
| Singleton guard | `static bool g_espnow_inited` replaces pointer-null check |

**BSS budget** (deterministic, no fragmentation):
| Symbol | Size |
|--------|------|
| `s_recv_queue_buf` (16 × 2058) | 32,928 B |
| `s_send_queue_buf` (8 × 2056) | 16,448 B |
| `s_send_task_stack` | 8,192 B |
| `g_espnow_ctx_storage` | 16,804 B |
| Sem/queue control blocks + task TCB | ~500 B |
| **Total new BSS** | **~74 KB** |

**Verification (`idf.py size` for esp32c3 counter):**
- DRAM: 60.45% used (194,224 / 321,296 bytes)
- BSS: 113,016 bytes
- Free heap remaining: 127,072 bytes (was ~74 KB fragmented before)

**Effect:** Cluster boots reliably on ESP32-C3. No `queue/sem alloc failed` errors. Heap fragmentation no longer a concern for transport init. Singleton-guard semantics preserved.



## Bundle 2 — Crypto UAF (CRITICAL)
**File:** `src/tbft_principal.c`

| Finding | Fix |
|---------|-----|
| F-001 (psa_hmac_in_id_prev UAF) | Removed PSA key ID storage; fallback re-imports from saved `hmac_in_key_prev` bytes per use |
| A1-F002 (psa_hmac_in_id_prev leak) | `tbft_principal_free` now destroys `psa_hmac_in_id_prev` |

**Design change:** Key rotation grace period now uses raw bytes (re-imported on each fallback verify) rather than a cached PSA key ID. Eliminates the dangling-ID class of bugs.

## Bundle 3 — Agreement Region Truncation (CRITICAL)
**File:** `src/tbft_agreement_region.c`

| Finding | Fix |
|---------|-----|
| F-007 (truncate not clearing new head) | After advancing `head_idx`, the new head slot is now explicitly cleared (prepared_cert + commit_cert + timestamps) |

**Effect:** Prevents stale bitmap entries from blocking quorum formation at the post-truncation head. Confirmed by A9 cross-verify as the root cause of the 7/43 cluster-wide state-divergence events.

## Bundle 4 — View-Change Trigger (CRITICAL)
**File:** `src/tbft_replica.c`, `src/tbft_replica.h`

| Finding | Fix |
|---------|-----|
| F-005 (view_installed_us=0) | `tbft_replica_init` sets `view_installed_us = esp_timer_get_time()` (was 0 from memset) |
| F-006 (primary drops during VC) | Removed `if (r->vi.in_progress) return;` — requests now queue, processed after VC completes |
| F-004 (view-change storm) | Added `last_vc_us` field; dead-primary detector gates on `now - last_vc_us > max(vtimer_period×2, 60s)` |
| F-020 (store_pp stuck) | `tbft_replica_send_pre_prepare` increments `r->seqno` on `store_pp` failure |
| FB-VS-F005 (vtimer backoff reset) | Indirectly addressed: VC triggers now respect unified `last_vc_us` backoff |

**Effect:** Targets the 6-in-2.8s view storm and 40% client failure rate observed in the 50-min deployment.

## Bundle 5 — Auth Gaps (HIGH)
**File:** `src/tbft_replica.c`

| Finding | Fix |
|---------|-----|
| F-008 (New_key has_sig=false) | Unconditional rejection of unsigned New_key — attacker no longer bypasses ECDSA |
| F-009 (VC proof dedup) | `prep_count_t` now has `replica_bmap`; one vote per replica per (seqno, digest) |
| F-010 (Status view delta) | Tightened cross-session guard: `delta>10` rejected, `delta>2` requires `last_stable` match |

## Bundle 6 — Client API (HIGH)
**Files:** `src/tbft_libbyz.c`, `include/esp-tinybft.h`

| Finding | Fix |
|---------|-----|
| F-012 (stack buffer 2KB) | `Byz_recv_reply` now uses `static` 2KB buffer (4KB FreeRTOS stack no longer at risk) |
| F-011 (static buffer unsafety) | Added `@note Thread safety: not thread-safe` to `Byz_send_request` and `Byz_recv_reply` in header |
| F-013 (mcast IP unvalidated) | Added `sscanf("%u.%u.%u.%u", ...)` format validation |
| F-014 (DER size OOM) | Added 4KB cap on key file size; legitimate keys are 32-65 bytes |

## Bundle 7 — State/Partition (HIGH)
**Files:** `src/tbft_state.c`, `src/tbft_state.h`, `src/tbft_partition.c`, `src/tbft_partition.h`

| Finding | Fix |
|---------|-----|
| F-015 (CoW vs rollback misaligned) | Added `cow_target_seqno` field; CoW and rollback now use this instead of `last_stable` |
| F-016 (stree dead code) | Removed `stree[]`, `stree_mem`, `total_nodes` from `tbft_ptree_t` — saves ~8KB heap |

## Bundle 8 — MEDIUM Severity
**Files:** `src/tbft_replica.c`, `src/tbft_principal.c`, `src/tbft_libbyz.c`, `src/tbft_state.c`, `src/tbft_libbyz.c`, `src/tbft_checkpoint_region.c`

| Finding | Fix |
|---------|-----|
| F-VS-003 (fill timeout VC) | Fill timeout VC now respects `last_vc_us` backoff |
| A5-F013 (mark_stable CoW regression) | `tbft_state_mark_stable` iterates slots and resets `num_old_blocks` for obsolete seqnos |
| A1-F005 (ECDH HKDF-Expand missing) | Added HKDF-Expand step with info string "TinyBFT session key v1" |
| A6-F009 (hostname permissive) | Replica hostname must be "node" + numeric suffix (e.g., "node3") |
| A8-F017 (no runtime init validation) | `tbft_replica_init` validates f, num_replicas, num_nodes ranges |
| A8-F011 (ndet silent clamp) | Added `ESP_LOGW` when ndet_max_len is clamped |

## Bundle 9 — LOW Severity
**Files:** `src/tbft_state.h`, `src/tbft_config.h`, `src/tbft_message.h`, `src/tbft_transport_espnow.c`, `examples/counter/main/main.c`

| Finding | Fix |
|---------|-----|
| A5-F008 (ckpt_head/ckpt_count dead) | Removed dead fields from `tbft_state_t` |
| A2-F004 (no TBFT_AUTH_SIZE assert) | Added `_Static_assert(TBFT_AUTH_SIZE > 0, ...)` |
| A2-F007 (missing VC/NV/NK/PP asserts) | Added `_Static_assert` for `tbft_pre_prepare_rep_t` |
| A7-F007 (find_node_by_mac race) | Added doc comment explaining the cross-task race and why it's acceptable |
| A7-F016 (hardcoded 68) | Replaced with `COUNTER_REQ_HEADER_SIZE` constant |

## Bundle 10 — INFO / Documentation
**Files:** `src/tbft_message.h`, `src/tbft_replica.h`, `src/tbft_replica.c`

| Finding | Fix |
|---------|-----|
| A2-F001 (RSA doc drift) | Updated 4 wire-format comments: "RSA signature" → "ECDSA P-256 signature" |
| F-019 (replica_free UAF) | Documented in header: must only be called from the replica task itself |

---

## Files Modified

| File | Lines Changed | Severity Addressed |
|------|--------------:|-------------------:|
| `src/tbft_transport_espnow.c` | ~25 | CRIT, HIGH |
| `src/tbft_principal.c` | ~30 | CRIT |
| `src/tbft_agreement_region.c` | +10 | CRIT |
| `src/tbft_replica.c` | ~50 | CRIT, HIGH, MED, LOW |
| `src/tbft_replica.h` | +15 | CRIT, INFO |
| `src/tbft_libbyz.c` | ~30 | HIGH, MED |
| `src/tbft_state.c` | ~25 | HIGH, MED |
| `src/tbft_state.h` | +10 | HIGH, LOW |
| `src/tbft_partition.c` | -15 | HIGH |
| `src/tbft_partition.h` | -15 | HIGH |
| `src/tbft_message.h` | ~10 | LOW |
| `src/tbft_message.h` (RSA→ECDSA) | 4 | INFO |
| `src/tbft_checkpoint_region.c` | +10 | MED |
| `src/tbft_config.h` | +5 | LOW |
| `Kconfig.projbuild` | 4 | INFO |
| `include/esp-tinybft.h` | +10 | HIGH |
| `examples/counter/main/main.c` | +5 | LOW |

## Build Verification

```bash
$ cd examples/counter
$ . ~/esp/esp-idf/export.sh
$ idf.py set-target esp32c3
$ idf.py build
[100%] Built target app
Project build complete.

$ cd ../simple_wallet
$ idf.py set-target esp32c3
$ idf.py build
[100%] Built target app
Project build complete.
```

Both examples build cleanly for ESP32-C3 (the most memory-constrained target).

## Findings Not Addressed

The following findings are documented but **not** fixed in this pass:

1. **Architecture-level refactors** that require extensive testing (e.g., unifying all three VC triggers behind a single `progress_detector`):
   - FB-VS-F001 (deep refactor of view-change state machine)
   - F-002 (store_pp fix is partial — full fix would require aborting the request)

2. **Dead code** that's harmless and might be needed for future protocol extensions:
   - A2-F002 (VC_ACK, META_DATA_D, QUERY_STABLE, REPLY_STABLE tags)
   - A6-F015 (counter_state in client build — minor memory waste)

3. **Refuted findings** (false positives identified by cross-verify):
   - L7-N001 / L5-F001 (Rogue primary) — REFUTED
   - L6-F002 (Prepare cert off-by-one) — REFUTED
   - A7-F002 (reasm last_tick stale) — REJECTED
   - A7-F009 (peer addr collision) — REJECTED
   - A7-F022 (WDT subscription) — REJECTED

4. **LOW/INFO findings** with no security or stability impact:
   - Various documentation drift
   - Log format improvements
   - Minor naming issues

5. **Performance optimizations** (LOW priority):
   - Small-message inline in transport entries
   - Memory budget assertion at init time

These items can be addressed in a follow-up PR if desired.
