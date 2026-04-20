# Implementation Report: esp-tinybft Component Improvements

## Changes Made

### `src/tbft_config.h` — Config Validation
- **No changes needed.** The `_Static_assert` for `TBFT_WINDOW_SIZE > TBFT_CHECKPOINT_INTERVAL` already exists at line 102-103. The compile-time guard was already in place, ensuring misconfigured builds fail early with a clear error message.

### `src/tbft_transport_espnow.c` — ESP-NOW Reliability & Diagnostic Logging
Added comprehensive diagnostic logging for packet drops and reassembly edge cases:

1. **Stale slot clearing** (`reasm_clear_stale`): Added `ESP_LOGD` logging when a stale slot is cleared with incomplete fragments, showing msg_id, fragment count, and age in milliseconds.

2. **Timeout-based eviction** (`reasm_find_or_alloc`): Added `ESP_LOGW` when a reassembly slot times out mid-reassembly, reporting the msg_id, progress (X/Y frags), and elapsed time before clearing.

3. **Slot exhaustion eviction** (`reasm_find_or_alloc`): Added `ESP_LOGW` when all 4 reassembly slots are full and the oldest partial message is evicted to make room for a new one. This helps diagnose high fragment loss or undersized `REASM_MAX_SLOTS`.

4. **Fragment feed rejections** (`reasm_feed`): Expanded all early-return guards with descriptive logging:
   - `ESP_LOGW` for msg_id / frag_total mismatch (corrupted or cross-talk fragments)
   - `ESP_LOGW` for out-of-range fragment index
   - `ESP_LOGD` for duplicate fragment detection (expected under retransmission)
   - `ESP_LOGW` for buffer overflow attempts

5. **Send path diagnostics** (`tbft_transport_send`):
   - `ESP_LOGD` for send rejection during shutdown / invalid transport state
   - `ESP_LOGW` for oversized message rejection
   - `ESP_LOGW` when send_queue is full and a message is dropped

6. **Send task diagnostics** (`espnow_send_task`):
   - `ESP_LOGW` when broadcast or unicast is skipped because a peer is not yet registered
   - Helps identify misconfigured peers or race conditions during startup

7. **Code organization**: Moved `now_ticks()` helper before `reasm_clear_stale()` to fix implicit declaration when stale-slot logging calls it.

### `src/tbft_node.c` and `src/tbft_replica.c` — Error Handling
- **No malloc/calloc/realloc calls exist** in either file. Both modules use static allocation (the replica struct embeds all regions by value) and stack-based message passing via the transport layer.
- All dynamic allocations in the codebase (in `tbft_libbyz.c`, `tbft_transport_espnow.c`, `tbft_transport_udp.c`, `tbft_partition.c`, `tbft_state.c`) already have proper null-check guards with cleanup paths.

## Rationale

The ESP-NOW reassembly system handles fragmented messages across a noisy wireless medium. Without diagnostic logging, partial message failures (timeout, eviction, duplicate, mismatch) are silent — the system simply drops data and retries at the BFT protocol level, making it impossible to distinguish between:
- A transient network issue (normal)
- A configuration problem (wrong `REASM_MAX_SLOTS`, too-short `REASM_TIMEOUT_MS`)
- A protocol-level bug (cross-message fragment injection)

Adding targeted logging at every drop point enables operators to diagnose reliability issues from serial output without modifying the protocol logic or adding test infrastructure.

Log levels were chosen deliberately:
- `ESP_LOGD` (Debug): Duplicate fragments and state info — noisy but useful during development
- `ESP_LOGW` (Warning): Actual data loss events — should be rare in production

## Known Limitations

1. **No per-message retry logic**: ESP-NOW send failures in `espnow_do_send` cause immediate message drop (return -1). The BFT protocol above handles retransmission via timers, but a transport-level retry counter could improve efficiency.

2. **Tick wrap-around in age calculations**: The stale-slot age calculation `(now_ticks() - s->last_tick) * portTICK_PERIOD_MS` works correctly for unsigned FreeRTOS tick wrap-around in subtraction, but the multiplication by `portTICK_PERIOD_MS` could overflow `uint32_t` for very long-lived stale entries (>49 days at 10ms tick). Practically irrelevant since `REASM_TIMEOUT_MS = 5000ms`.

3. **Reassembly slot limit is fixed at 4** (`REASM_MAX_SLOTS`): With 4 concurrent reassembly sessions, burst traffic from >4 nodes simultaneously sending fragmented messages will cause evictions. Making this configurable via Kconfig would improve flexibility.

4. **No end-to-end fragment delivery metrics**: There is no counter tracking total fragments received vs. successfully reassembled, which would be useful for computing a fragment loss rate.

5. **Send queue overflow**: The send queue depth is 4 (`SEND_QUEUE_DEPTH`). Under heavy broadcast load (e.g., Prepare messages from all replicas), this can fill quickly. Messages are dropped with logging but no backpressure is signaled to the replica layer.
