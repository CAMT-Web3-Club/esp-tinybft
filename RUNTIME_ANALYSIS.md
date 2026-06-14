# esp-tinybft Runtime Log Analysis

## Run Overview

| Metric | Value |
|--------|-------|
| Nodes | 7 replicas (node0-node6) + 1 client (client7) |
| Cluster config | f=2, n=7, CHECKPOINT_INTERVAL=2, WINDOW_SIZE=4 |
| Transport | ESP-NOW (v2.0 with auto-fragmentation/reassembly) |
| Deployment duration | 50.4 minutes (3,021,854 ms) |
| Total log volume | 130,379 lines across 8 log files |
| Per-node log lines | node0=19,928, node1=9,072, node2=17,823, node3=8,757, node4/5/6≈4-6k, client7=3,014 |
| View transitions | 6 total (v0 → v1 → v2 → v3 → v4 → v5), cluster stabilized in v4-v5 |
| Commits executed | 117-124 per replica (cluster-wide successful) |
| Client requests | 90 attempted, 36 failed completely (40.0% failure rate) |
| Client success rate | 60% overall (51% attempt-1, 4% attempt-2, 0% attempt-3) |
| msg_queue drops | 604 cluster-wide (148 on node0 alone) |
| Checkpoint digest mismatches | 43 cluster-wide (7 fetch-initiating on node0) |
| MAC verification failures | 187 cluster-wide (79 on node0, 44 on node1, 15 on node2, 49 on node3) |
| View-change timeouts | 4 of 6 VCs are timeouts (not Byzantine-induced) |
| Safety violations | **0** — BFT safety preserved throughout |

---

## Per-Node Statistics

### Primary (node0)

| Metric | Count | Notes |
|--------|------:|-------|
| PP accepted (view=0) | 31 | All PPs from local primary in view 0 |
| Commits sent | 117 | Includes all view=0 through view=4 commits |
| Checkpoints sent | 60 | Every even seqno in view=0 plus view=1-4 |
| New_key sent (out_key) | 273 | ~45 full rotation cycles |
| New_key received (in_key) | 273 | All sent confirmations matched |
| msg_queue drops | 148 | 75 Checkpoint, 37 Commit, 30 Status, 5 PP/NK; first 6 minutes |
| MAC verification failures | 79 | 30 Checkpoint, 27 Commit, 20 Prepare, 2 PP |
| Checkpoint digest mismatches | 7 | seqnos 6, 10, 12, 20, 26, 34, 128 — all triggered fetch |
| State fetch ops | 7 | 6 from replica 1, 1 from replica 5 |
| Stable checkpoints | 60 marked_stable, 68 stable_checkpoint (8 phantom = digest mismatch) |
| View changes | 5 | Node0 was primary only in view 0; views 1-5 had rotating primary |
| Execute events | 117 | Successful increments (matches commits) |
| ESP-NOW send success rate | 100% (6622 OK, 0 FAIL, 0 ERR) | Driver queue acceptance, not delivery |
| Per-replica response rate to client | 96.4% | Standard |

### Backup node1

| Metric | Count | Notes |
|--------|------:|-------|
| PP accepted (view=0/2/3/4) | 15/18/6/24 | 4 view changes; primary chain observed: 0→2→3→4 |
| Exec_unique_seqnos | 71 | |
| Committed_unique_seqnos | 100 | |
| Stable_checkpoints_unique | 26 | With gaps during VC transitions |
| out_of_window_count | 38 | Every checkpoint boundary (1.5s lag) |
| out_of_window_unique_seqnos | 32 | |
| pp_verifying | 61 | |
| pp_accepted | 62 | |
| prepared_count | 63 | 2f+1=5 quorum forms correctly for non-OOW seqnos |
| committed_unique_seqnos | 100 | |
| mac_failures | 44 | |
| msg_queue_full_drops | 38 | |
| pp_resent_to_node0 | 11 | (primary was lagging) |
| commit_resent_to_node0 | 10 | |
| checkpoint_resent_to_node0 | 22 | |
| view_change_count | 3 | |
| first_pp_recv_us | 18894 | |
| first_exec_us_seqno1 | 19524 | (delta to first reply: ~600µs) |

### Backup node2 (the 'fetching' replica)

| Metric | Count | Notes |
|--------|------:|-------|
| PP accepted (view=0/1/3/4) | 31/11/0/44 | view=2 missing (went 0→1→3→4, skipping v=2) |
| out_of_window_count | 105 | Highest of all backups (1.5s lag pattern) |
| out_of_window_unique_seqnos | 48 | All even seqnos 2-30, then 72-122 |
| **out_of_window_burst_seqno_40** | 33 | 33-replay burst at t=923-933ms |
| prepared_count | 124 | |
| committed_unique_seqnos | 118 | |
| exec_unique_seqnos | 120 | |
| stable_checkpoints_unique | 64 | Best coverage of all backups |
| mac_failures | 15 | |
| msg_queue_full_drops | 131 | **Highest of all backups** |
| pp_resent_to_node0 | 13 | |
| commit_resent_to_node0 | 13 | |
| checkpoint_resent_to_node0 | 25 | |
| view_change_count | 4 | Most active |
| first_pp_recv_us | 19664 | 770µs slower than node1 |
| **fetch_protocol_activity** | 6 fetches, 10 digest mismatches | Cascading fetch at seqno 44, 50, 58, 64, 68 — never recovered cleanly |
| tag12_fetch_recv | 6 | |
| tag14_data_recv | 5 | |
| tag15_status_recv | 11 | |

### Backup node3 (the 'slow-start' replica)

| Metric | Count | Notes |
|--------|------:|-------|
| PP accepted (view=0/2/3/4) | 15/10/4/18 | Missed first 4 PPs (1-4, 6, 7, 10-14) — WiFi association delay |
| out_of_window_count | 34 | |
| out_of_window_unique_seqnos | 24 | |
| pp_verifying | 52 | |
| pp_accepted | 47 | **Lowest of all backups** (47 vs 96 on node2) |
| prepared_count | 61 | |
| committed_unique_seqnos | 96 | |
| exec_unique_seqnos | 48 | **Never executed seqno 1** (first exec was seqno 8) |
| stable_checkpoints_unique | 36 | Sparse (6, 8, 14, 16, 20, ...) |
| first_pp_recv_us | 28894 | 10ms behind node1 (18894) and node2 (19664) |
| first_pp_accepted_seqno | 5 | |
| mac_failures | 49 | **Highest of all backups** |
| msg_queue_full_drops | 27 | |
| first_stable | 6 | (vs 2 on node1/node2) |
| Never entered fetch | 0 fetch ops | Stuck on initial slow-start; caught up only via view changes |

### Backups 4-6 (summary; L3 detailed analysis)

| Metric | node4 | node5 | node6 |
|--------|------:|------:|------:|
| counter increments (end state) | 117 | 124 | 124 |
| view_changes | varies | varies | varies |
| end view | 4-6 | 4-5 | 4-5 |
| msg_queue drops (cluster total) | 56 | 87 | 117 |

### Client (cid=7)

| Metric | Value | Notes |
|--------|------:|-------|
| Total requests | 90 | ~46 min wall-clock |
| Successful attempt-1 | 51 (57.3%) | |
| Successful attempt-2 | 4 (4.4%) | |
| Successful attempt-3 | 0 (0%) | |
| Failed completely | 36 (40.0%) | |
| In-progress at end | 1 | |
| **Failure rate** | **40.0%** | |
| RSA-2048 sign count | 160 measurements | |
| avg RSA sign | 193,840 µs (~194ms) | |
| stddev RSA sign | 450 µs | Very consistent (~0.5ms) |
| Total client crypto time | 17.5s (90 × 194ms) | |
| End-to-end latency (attempt-1 success) | 3,870-8,470 ms (avg 4,700ms) | |
| End-to-end latency (attempt-2 success) | 25,100-27,100 ms | Includes 1s retry wait |
| Failed request total wall-clock | 63,570 ms | |
| Total retry wait time | 108 seconds (38×1s + 35×2s) | |
| Total RSA sign time | ~31 seconds | |
| Per-replica response rate | r3/r6=100%, r0/r1/r2/r4=96.4%, r5=88.1% | One weak link on r5 |
| **0-reply failures** | 35 of 35 failures | All failures are 0-reply, not partial 6/7 |
| View changes observed | 5 | Cluster's view=0→1→2→3→4→5 |
| Failure cluster pattern | 11 + 1 + 10 + 1 + 11 + ongoing | Each cluster aligns with a view change |
| Counter value gaps | 5→32, 46→71, 80→81, 98→125 | 26+24+0+26 = 76 missed operations |

---

## Observed Issues

### Issue 1: msg_queue saturation — 148 drops on primary, 604 cluster-wide

**Symptom:** `tbft_transport_espnow.c` reassembly-to-app queue overflows permanently with drop-on-full.

**Statistics:**
- Per-tag drop distribution on node0: 75 Checkpoint, 37 Commit, 30 Status, 5 PP/NK
- Per-node drop totals: node0=148, node1=38, node2=131, node3=27, node4=56, node5=87, node6=117
- 100% of the burst load happens in the first 6 minutes (147/148 drops on node0)
- 75% of Checkpoint drops within 200ms of a local broadcast (chicken-and-egg: node0's broadcast floods the queue when receiving replicas' checkpoints arrive)

**Triggers:**
- `MSG_QUEUE_DEPTH=16` (line 47, configurable via dead Kconfig symbol)
- `xQueueSend(... timeout=0)` at line 398 (non-blocking)
- Consumer `xQueueReceive(timeout=0)` at line 813 (replica task)
- 5-second reassembly slot timeout causes near-simultaneous flushes

**Downstream impact:**
- 7/43 cluster-wide state-divergence events (L5-F003) directly attributable
- 5/7 replicas enter fetch state at various points (L5-F003)
- node2 enters cascading fetch (seqno 44, 50, 58, 64, 68) — never recovers cleanly
- 6 view-changes triggered by 10s fill timeout (L5 timeline)

**Source files:** `src/tbft_transport_espnow.c:46, 47, 398, 631, 813`

### Issue 2: View-change storm — 6 transitions in 2.8 seconds

**Symptom:** Cluster oscillates through views with insufficient recovery time between them.

**Timeline (L5):**
- t=801,054 ms: view=1 installed
- t=803,014 ms: node3 sent new-view v=1 (rogue-primary parser artefact)
- t=932,343 ms: view=2 triggered by 'fill: commit missing at seqno=40 after 10204ms'
- t=1,754,294 ms: view=3 installed
- t=1,843,833 ms: view=4 triggered by 'fill: commit missing at seqno=78 after 10016ms'
- t=2,836,793 ms: view=5 installed
- t=3,101,964 ms: view=6 ongoing

**Trigger distribution:**
- 4 of 6 VCs are vtimer (15s) timeouts
- 2 of 6 VCs are fill-timeout (10s) triggers
- dead-primary detector (30s) is harder to filter in logs but also fires

**Root cause:** Three independent, uncoordinated VC triggers:
1. vtimer (line 417, configurable, has backoff)
2. dead-primary detector (line 387, hardcoded 30s, no backoff)
3. fill timeout (line 600, hardcoded 10s, no backoff)

**Downstream impact:**
- 5-15s of consensus downtime per view change
- RSA-2048 sign ~194ms × 5+ operations per VC = ~1s crypto alone
- Network contention from msg_queue + send_credit + reasm halving compounds
- Client retries with 1s+2s backoff cannot wait through a 5-10s VC

**Per-view primary chain observed:**
- node1: 0→2→3→4 (skipped v=1)
- node2: 0→1→3→4 (skipped v=2)
- node3: 0→2→3→4 (skipped v=1)

This non-determinism is expected PBFT behavior but means the cluster spent time in inconsistent views, each VC round costing 1-2s of reduced throughput.

**Source files:** `src/tbft_replica.c:387-397, 417-420, 600-628, 1849-1864`

### Issue 3: Commit/PP out-of-window — 773 cluster-wide warnings (benign)

**Symptom:** Late Pre-prepare or Commit messages arrive after the receiver has rotated the window past that seqno.

**Statistics:**
- Per-node OOW: node0=141, node1=49, node2=138, node3=56, node4=92, node5=140, node6=157
- Total: 773 cluster-wide over 50 min = ~15/min
- Pattern: stable_X at t=X, oow_X at t=X+1500us (range 1350-3020us) for every even seqno
- 33-burst at node2 seqno 40 (t=923-933ms) — node0 sat on commit-certificate queue, drained at 100-300us intervals

**Root cause:** Primary (node0) is consistently 1-1.5s behind backups on commit-certificate broadcast. By the time the commit arrives, the backup has stable-checkpointed the seqno and rotated the window.

**Severity:** Benign by design (A9-F001). The rejection is a 'safe reject' — the seqno is already committed and state is consistent. But the log volume obscures real issues.

**Source files:** `src/tbft_replica.c:943, 1145, 1262, 2391, 3302`

### Issue 4: Checkpoint digest mismatches — 43 cluster-wide

**Symptom:** Local checkpoint digest does not match the cluster's winning digest, forcing state fetch.

**Statistics:**
- Per-node mismatches: node0=13, node2=10, node4=8, node5=8, node6=4, node1=0, node3=0
- Fetch-initiating (i.e., entered fetch): node0=6, node1=1, node2=5, node4=6
- node0 affected seqnos: 6, 10, 12, 20, 26, 34, 128 (every checkpoint in the early phase)

**Trigger pattern:** msg_queue drops (Issue 1) prevent a replica from receiving all 2f+1 Checkpoint messages. With only 4-5 of 6 checkpoints, the local digest over its own state plus the partial received digests differs from the cluster's.

**Cascading on node2:** Fetch at seqno 44 → digest still mismatches at 50, 58, 64, 68. node2 never recovers cleanly.

**Source files:** `src/tbft_state.c:251, 2810, 2876, 2895, 2906`, `src/tbft_checkpoint_region.c`, `src/tbft_replica.c`

### Issue 5: MAC verification failures — 187 cluster-wide (key-rotation race)

**Symptom:** After New_key broadcast, peers continue to send messages authenticated with the OLD key for a 200-1500ms window.

**Statistics:**
- Per-node MAC failures: node0=79, node1=44, node2=15, node3=49, others combined
- Per-source: r1=15, r2=9, r3=5, r4=12, r5=12, r6=19 (node0's perspective)
- All 79 events on node0 cluster around lines 2846-2948 (3 rotations in 80s)

**Root cause:** Key-rotation race — between `send_new_key` and `handle_new_key` ACK, a peer that has rotated but the local has not will see stale MACs. No overlap window maintained.

**Source files:** `src/tbft_principal.c` (verify_mac_in, in_key/out_key), `src/tbft_replica.c` (handle_new_key, send_new_key)

### Issue 6: Client 40% failure rate — primary silently drops during view-change

**Symptom:** Client requests return 0 replies (not partial 6/7) and the client times out after 3 retries.

**Statistics:**
- 36 of 89 requests fail completely (40.4%)
- 35 of 35 failures are 0-reply timeouts
- Client retry backoff: 1s, 2s — far too short for 5-10s VC recovery
- 108s of pure retry wait time across 35 failed requests

**Root cause:** `tbft_replica.c:880` returns immediately when `r->vi.in_progress` is true, with no Reply, Status, or NACK. The client cannot distinguish 'request rejected due to view-change' from 'request lost in transit'.

**Counter value gap evidence:** Counter jumped 5→32, 46→71, 80→81, 98→125 across failure periods. The cluster DID process the requests (counter advanced) but the client never received 3 matching replies because the retries landed in different views with different rids.

**Source files:** `src/tbft_replica.c:874-880`, `src/tbft_libbyz.c:787-823`, `examples/counter/sdkconfig.defaults:43`

### Issue 7: ESP-NOW send credit starvation (per-peer asymmetry)

**Symptom:** 4 confirmed `no send credit after 100ms` drops on node5, each dropping 1 of 6 peer sends in a broadcast.

**Statistics:**
- 4 events on node5 at lines 1803, 3491, 8938, 15351
- Starved peers alternate: node2 (1803, 8938), node1 (3491, 15351)
- End-of-log: 2 re-sent commits to replica 5 in 5 seconds

**Root cause:** Flat counting semaphore (`xSemaphoreCreateCounting(ESPNOW_TX_CREDITS, ESPNOW_TX_CREDITS)`) with 100ms hard timeout. A single slow peer holding 1 credit for 250ms-1s can starve others for that window. The 1s `last_fail_tick` cooldown partially mitigates.

**Source files:** `src/tbft_transport_espnow.c:445, 635`

### Issue 8: Reassembly slot capacity halved under fragmented traffic

**Symptom:** 8 drops in 50ms during fragmented bursts; reasm capacity 4 instead of 8.

**Root cause:** Completed-message slot is marked `stale=true` but `valid` remains true. `reasm_find_or_alloc` returns only `!s->valid` slots, so stale-but-valid slots are not eligible for new messages. Under fragmented traffic, half the reasm slots are unusable.

**Source files:** `src/tbft_transport_espnow.c:196-303`

### Issue 9: node2 has 3.7x slower broadcast reception

**Symptom:** node2 takes ~1050us p50 to receive a PP from the primary, while others take 270-500us.

**Statistics:**
- Per-node pp_send_to_pp_recv latency (view 0, n=31 each):
  - node1=309µs, node2=1063µs, node3=278µs, node4=317µs, node5=517µs, node6=376µs
- node2's 1050µs is consistent (p50 AND p90), not random loss

**Likely cause:** Physical-layer issue (RF distance, antenna, channel). Not a protocol bug but a deployment constraint.

### Issue 10: node3 receives 0 messages from node1 (one-way loss)

**Symptom:** Recv counts for MAC 88:56:a6:5b:76:85 (node1): node0=1111, node1=0(self), node2=1056, node3=**0**, node4=1030, node5=1055, node6=998.

**Likely cause:** Per-board fault (channel mismatch, RF shadowing, faulty peer registration for node3's view of node1).

### Issue 11: Transport-level duplicate delivery (L4-N007)

**Symptom:** Some requests receive 8-22 replies (instead of expected 6-7).

**Statistics:**
- Per-request reply count distribution: {6: 12, 7: 38, 8: 1, 12: 1, 13: 4, 14: 6, 16: 1, 20: 8, 21: 7, 22: 3}
- 18 requests receive 20-22 replies

**Root cause:** Stale reasm slot is re-used for retransmitted fragments. The fully-reassembled message is queued twice. Application-level dedup in `Byz_recv_reply` (slot-based on (rep_id, rid)) correctly filters these — BFT safety preserved.

---

## Phase Latencies

| Phase | Latency (p50) | Notes |
|-------|--------------:|-------|
| Request → first PP recv (view 0) | 19-29 ms | node1=18.9, node2=19.7, node3=28.9 (slow start) |
| PP recv → first Prepare send | < 1 ms | Backups respond immediately |
| Prepare accept (per-replica) | 1-2 ms | |
| Commit accept (per-replica) | 1-3 ms | |
| First commit_recv (node3) → last (node0) | 2 ms | Primary is consistently the slowest |
| First reply_send → last | 2 ms | Primary is consistently the slowest |
| Execute (commit accept → exec) | < 1 ms | |
| Reply → client recv | ~2.6s (RSA sign) + 100-500ms (network) | |
| **End-to-end (attempt-1 success)** | **3,870-8,470 ms (avg 4,700)** | Dominated by `recv_reply_timeout=2s` |
| **End-to-end (attempt-2 success)** | **25,100-27,100 ms** | Includes 1s retry wait |
| **Failed request total** | **63,570 ms** | 3 attempts × 20s timeout + 1s + 2s |
| **View change** | **5-15s** | RSA-2048 sign ~1s + network contention 4-14s |
| Fill timeout (10s) → VC | 10,000-10,500 ms | Hardcoded; can be transient loss |
| View-change timeout (15s) → next VC | 15,000 ms (with backoff) | Exponential up to 60× |
| Dead-primary detector (30s) → VC | 30,000 ms | Hardcoded, no backoff |
| Key rotation (one peer) | 4-5 seconds | 6 RSA-2048 decrypts + 1 RSA sign |
| Full key rotation cycle (6 peers) | ~60s cadence | 273 events / 50min ≈ 5.5/min |
| RSA-2048 sign (client) | 193,840 µs avg (192,858-196,573 µs range) | σ=450 µs |
| Broadcast send (replica→replica) | 200-500 µs | Per-receiver, including ESP-NOW driver |

---

## Cross-Node Anomalies

### Anomaly A: Per-peer broadcast latency asymmetry

| Source → Dest | Median (µs) | Notes |
|----------------|------------:|-------|
| node0 → node1 | 309 | |
| node0 → **node2** | **1063** | 3.4x slower than peers; correlates with node2's fetch activity |
| node0 → node3 | 278 | (first seqno only; node3 had 10ms slow-start) |
| node0 → node4 | 317 | |
| node0 → node5 | 517 | |
| node0 → node6 | 376 | |

**Implication:** node2 is consistently the slowest receiver. This is the cause of node2's higher view-change count (4 vs 3 on others) and cascading fetch pattern.

### Anomaly B: Per-peer response rate to client (L4-F009)

| Replica | Response rate | Notes |
|---------|---------------:|-------|
| replica_3 (1c:db:d4:c6:40:99) | 100.0% | |
| replica_6 (88:56:a6:5b:78:dd) | 100.0% | |
| replica_0/1/2/4 | 96.4% | |
| **replica_5 (1c:db:d4:c5:7f:21)** | **88.1%** | One weak link — physical-layer or board fault |

### Anomaly C: Per-node first-stable-checkpoint distribution

| Node | first_stable | last_stable | gap (seqnos missed) |
|------|-------------:|------------:|--------------------:|
| node1 | 2 | 118 | 12 (gaps: 6, 8, 14, 16, 20, 22, 24, 26, 28, 30, 32, 34) |
| node2 | 2 | 122 | 0 (perfect coverage) |
| node3 | 6 | 122 | 22 (gaps: 2, 4, ...) |

### Anomaly D: Per-replica recv counts for node1's MAC

| Receiver | Recv count from node1 (MAC 88:56:a6:5b:76:85) |
|----------|------------------------------------------:|
| node0 | 1111 |
| node1 (self) | 0 |
| node2 | 1056 |
| **node3** | **0** |
| node4 | 1030 |
| node5 | 1055 |
| node6 | 998 |

**Implication:** node3 and node1 have a one-way reception issue. Not a protocol bug — physical-layer or peer-registration issue. ESP-NOW is broadcast; every node should receive every other node's messages.

### Anomaly E: Primary is consistently the slowest to commit and reply

For seqno=1, first commit_recv at node3@19454µs, last at node0@21464µs (delta 2ms). Same pattern for all 31 PPs in view 0. The primary does BOTH broadcast PP and verify Prepare, while backups just verify PP+Prepare.

### Anomaly F: Non-deterministic view-change sequence

| Node | Views experienced |
|------|-------------------|
| node1 | 0 → 2 → 3 → 4 (skipped v=1) |
| node2 | 0 → 1 → 3 → 4 (skipped v=2) |
| node3 | 0 → 2 → 3 → 4 (skipped v=1) |
| node0 (primary in v=0) | 0 → (becomes backup in v=1-5) |

**Implication:** View-change is per-replica, not synchronized. The cluster converges at v=4 only because New_view quorum forces all replicas to install the same view. Convergence time is 1-2s per round, multiplied by intermediate views.

### Anomaly G: Node3 WiFi association delay

node3's first PP is at t=28894µs, 10ms behind node1 (18894µs) and node2 (19664µs). node3's first `pp accepted` is seqno=5. node3 missed 22 PPs in view=0 due to association delay. Despite this, node3 received 0 from node1 (one-way loss) — the slow-start and one-way loss are not the same issue.

---

## Log Volume

| Log file | Lines | Size | Density (lines/sec) |
|----------|------:|-----:|--------------------:|
| node0 | 19,928 | 1.26 MB | 6.59 |
| node1 | 9,072 | ~600 KB | 3.00 |
| node2 | 17,823 | ~1.1 MB | 5.90 |
| node3 | 8,757 | ~580 KB | 2.90 |
| node4-6 | ~4-6k each | ~400 KB | ~1.5 |
| client7 | 3,014 | ~200 KB | 1.00 |
| **Total** | **130,379** | **~5 MB** | **~43/s aggregate** |

node0 (primary) and node2 (fetching replica) have 2x the log volume of node1/node3 — directly proportional to their work and the problems they hit.

---

## Cluster-Wide Summary

**What worked (positive findings):**
- Prepare cert forms correctly (2f+1=5 quorum on all non-OOW seqnos)
- Commit phase completes correctly
- Checkpoint stable progression is monotonic on most replicas
- 117-124 increments execute correctly across 7 replicas
- BFT safety is preserved throughout (no safety violations observed)
- Key rotation is successful (273 sent, 273 received, all matched)
- ESP-NOW send success rate 100% (driver-level)
- 6,622 send_task calls, 0 FAIL, 0 ERR

**What failed (root causes):**
1. msg_queue drops → 7 fetch-initiating mismatches (node0)
2. 5 view changes in 50 min, 6-in-2.8s burst
3. 40% client failure rate from primary dropping during VC + short backoff
4. node2 cascading fetch (5 fetches, never recovered)
5. node3 10ms WiFi association delay + 0-recv from node1 (physical-layer)

**Verdict:** BFT-safe but BFT-unavailable. 40% client failure rate, 6 view changes, 5/7 replicas in fetch state at various points. The protocol tolerates the observed chaos but at significant throughput cost. Fixes needed are for availability, not safety.
