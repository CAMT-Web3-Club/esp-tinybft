# Cross-Swarm Feedback: Runtime → Code Review

## How Swarm 2 Findings Informed Swarm 1

The runtime log analysis (Swarm 2) and the static code review (Swarm 1) operated largely in parallel, but the feedback loop was essential for:

1. **Severity upgrades** — several S1 findings that were correctly identified but under-rated by static review were confirmed CRITICAL/HIGH by runtime evidence.
2. **Refutations** — one S2 finding (L7's "rogue primary" CRITICAL claim) was a log-parser artefact, not a real bug. Feedback loop re-review prevented a wild goose chase.
3. **New findings** — feedback loop uncovered 4 critical/serious findings that neither S1 nor S2 had surfaced (msg_queue drop policy, view-storm trigger multiplicity, primary drops in VC, transport duplicate delivery).

---

## Severity Upgrades

| S1 finding | Original S1 severity | New severity | Runtime evidence | Concrete action |
|-----------|--------------------:|-------------:|------------------|-----------------|
| **A7-F004** (msg_queue depth=16) | HIGH | **CRITICAL** | L1-F001: 148 drops on node0; L5-F005: 604 cluster-wide. 75% of node0's Checkpoint drops within 200ms of a local broadcast. Directly causes 7/43 state-divergence events. | Increase `MSG_QUEUE_DEPTH` from 16 to 32 (memory cost ~66 KB; ESP32-C3 80 KB free heap is tight). Change `xQueueSend` timeout from 0 to 10ms. Distinguish fragment-drop (acceptable) from reassembled-drop (never acceptable). |
| **A7-F006** (send credit starvation) | HIGH | **CRITICAL** | L3-F002: 4 `no send credit after 100ms` drops on node5 (lines 1803, 3491, 8938, 15351). Per-peer starvation pattern. BFT 2f+1 quorum still reached (1 of 6 peers dropped), so this is availability-degrading not safety-breaking. | Per-peer credit tracking (6 per-peer semaphores). Increase `ESPNOW_CREDIT_TIMEOUT_MS` from 100 to 500ms. Pre-check `last_fail_tick` per peer before acquiring credit. Add per-peer metric counters. |
| **A9-F002** (store_pp stuck) | MEDIUM | **HIGH** | L5-F010: 'fill: prepare ok but commit missing at seqno=40 after 10204ms' on node4, followed by 'sent view-change to view 2'. The 10s stall is the fill timeout (line 600). When `tbft_ar_store_pp` fails (line 2480-2484), the same seqno is retried indefinitely. The feedback loop confirmed: rqueue_push → send_pre_prepare → store_pp fails → seqno unchanged → primary stalled → vtimer fires → VC → 5-10s downtime. | On `tbft_ar_store_pp` failure, increment `r->seqno` before returning. Optionally, on persistent failure (3 consecutive), trigger a view change immediately. |
| **A8-F001** (view_installed_us=0 init) | HIGH | **CRITICAL** | L1-F004: 4 view-change timeouts on node0 in the 50-min run. L7 confirms the 30s gap is real for any direct caller of `tbft_replica_init` (the only current caller `Byz_init_replica` patches the value at `tbft_libbyz.c:985`, but the public API contract is unsafe). | Set `r->view_installed_us = INT64_MAX` in `tbft_replica_init` (sentinel meaning "view not yet installed"), or add a `bool view_installed_valid`. The dead-primary check should gate on this flag. |
| **A4-F001** (truncate not clearing new head) | MEDIUM | **CRITICAL** | A9 cross-verifier (CV_A4_by_A9) proved the DoS chain: every checkpoint → mark_stable(seqno) → tbft_ar_truncate(ar, seqno+1) → new head slot retains previous-window pc.bmap. For n=4 (f=1, threshold=3), the local node being blocked leaves at most 2 honest prepare-contributors — below threshold. L7 correlation: 7/43 cluster-wide state-divergence events are downstream of this bug. | After advancing `head_idx`, also explicitly clear the slot at the new head: `tbft_prepared_cert_clear(...)`; `tbft_commit_cert_clear(...)`; reset `commit_sent_us` and `fill_sent_us`. 4-line fix. |
| **A7-F003** (reasm slot lifetime) | MEDIUM | **HIGH** | L1-F001 burst pattern: 8 drops in 50ms during fragmented bursts at seqno=3 transition. L2-F004: 131 drops on node2 (highest of all backups), correlating with node2's fetch activity producing large fragmented tag=12/14 messages. Reasm capacity 4 instead of 8 directly enables the msg_queue drops. | In `reasm_find_or_alloc`, also return slots with `s->stale == true` (call `reasm_clear_stale` first). Add a quick clear at the top of `reasm_find_or_alloc` when called from `tbft_transport_recv` (line 804). |
| **A7-F005** (SEND_QUEUE_DEPTH=8) | MEDIUM | **HIGH** | L1-F007: 45+ complete New_key rotations; L1-F008: 2 re-sent commits + 2 re-sent checkpoints in 5s at end of log. The view-change protocol broadcasts ~14 large messages (7 View_change unicast + 1 New_view broadcast + 1 New_key broadcast), easily overflowing the 8-deep queue. | Increase `SEND_QUEUE_DEPTH` to 16-32 (configurable via Kconfig). Memory cost: 16 × 2056 = 33 KB, 32 × 2056 = 66 KB. ESP32-C3 80 KB heap is tight; 16 is safer. |

### Cross-verify-driven severity adjustments (not direct runtime)

| S1 finding | Original | New | Rationale |
|------------|---------:|----:|-----------|
| A1-F001 (crypto UAF) | CRITICAL | CRITICAL | Pre-existing; confirmed by A2 CV (no downgrade). |
| A1-F003 (HKDF fallback) | HIGH | INFO (downgrade) | A2 CV: design is correct; only F001 (use-after-free) breaks it. The proposed "try either order" fix would actually weaken security. |
| A1-F004 (RSA doc drift) | INFO | LOW (upgrade) | A2 CV: TBFT_SIG_SIZE=64 vs comment says 256; a future maintainer 'fixing' the value to 256 would break every static_assert. |
| A5-F001 (CoW correctness) | HIGH | HIGH | A10 CV: live impact lower (rollback is dead code in this codebase) but bug is real if rollback were ever called. |
| A5-F003 (rollback) | HIGH | HIGH | A10 CV: live impact low (dead code path) but bug is real. |
| A5-F004 (CoW GC) | HIGH | MEDIUM (downgrade) | A10 CV: memory bound exists at NUM_CKPT_SLOTS, but stale num_old_blocks remains a real correctness issue. |
| A2-F002 (dead protocol tags) | MEDIUM | MEDIUM | A1 CV: real semantic issue; PBFT features (VC_ACK, META_DATA_D) are not implemented. |
| A2-F008 (nonce field) | MEDIUM | INFO (downgrade) | A1 CV: `esp_fill_random()` at tbft_replica.c:3118 confirms the nonce is properly random. |
| A3-F002 (TBFT_CERT_MAX_VALS comment) | LOW | LOW | A10 CV: comment is imprecise but the bound is correct PBFT semantics. |
| A6-F002 (stack overflow) | HIGH | HIGH | A8 CV: examples use 8 KB stack so not currently triggered; risk is real for non-default config. |
| A6-F008 (stale-reply drain) | MEDIUM | REJECTED | A8 CV: behavior is correct; the comment accurately describes the design. |
| A6-F014 (ndet overflow) | LOW | INFO (downgrade) | A8 CV: the clamp at tbft_replica.c:178-179 prevents overflow; the ndet_max_len documentation gap is real but no overflow. |
| A6-F019 (log format specifier) | LOW | REJECTED | A8 CV: ESP-IDF logging is safe; user strings are arguments, not format strings. |
| A7-F001 (frag_idx >= frag_total dead code) | LOW | LOW | A8 CV: line 270 IS a meaningful defensive check; A7's "dead code" claim was wrong. |
| A7-F002 (reasm last_tick stale) | MEDIUM | REJECTED | A8 CV: `reasm_feed` always updates `last_tick` at line 260 (init) or 292 (write); the "stale" claim is wrong. |
| A7-F004 (msg_queue undersized) | HIGH | (see CRITICAL upgrade above) | |
| A7-F008 (msg_id wraparound) | LOW | LOW | A8 CV: 5s reasm timeout + match-on-(msg_id, src_mac) prevents silent drop; not "medium". |
| A7-F009 (peer addr collision) | LOW | REJECTED | A8 CV: AP MAC transformation is bijective; the "collision" claim is wrong. |
| A7-F022 (WDT for replica) | LOW | REJECTED | A8 CV: replica task IS subscribed to WDT via tbft_replica_run at line 338; A7 missed the call chain. |
| A8-F003 (start_fetch clobber) | MEDIUM | LOW (downgrade) | A6 CV: actual call-site discipline prevents the issue in current code. |
| A8-F017 (init validation) | LOW | INFO/LOW | A6 CV: parse_config already warns about PBFT invariant violations. |

---

## Refuted Findings

### L7-N001 / L5-F001 — Rogue primary protocol bug (REFUTED by FB-RoguePrimary)

| Field | Detail |
|-------|--------|
| S2 claim | L7-N001 (CRITICAL): "Rogue primary protocol bug: view mod num_replicas uses wrong denominator (num_nodes vs num_replicas)" |
| Evidence cited | L5-F001: node3 sent 8 PPs in view 1 (should be node1); node4 sent 10 PPs in view 3 (should be node3). L2-F015: per-backup view sequences differ (node2: 0→1→3→4; node1/3: 0→2→3→4). |
| Refuted by | FB-RoguePrimary (7 findings, all REFUTED) |
| Refutation evidence | (1) The primary-selection formula is `v % num_replicas` (inline at `tbft_node.h:170-173`, used identically at 14 call sites in `tbft_replica.c`). `num_replicas` is the runtime count (7 for counter example), NOT the compile-time cap. (2) `tbft_node_primary()` is the ONLY function computing primary; no other formula exists. (3) `handle_new_view` installs the primary strictly from the formula at line 1773 — New_view message contains no 'new_primary_id' field that could override it. (4) `handle_pre_prepare` MAC-verifies every PP against `expected_primary`'s session key at line 970-994; a wrong-primary PP is rejected before any state change. (5) The mathematical claim is self-contradictory: for views 1 and 3, all denominator variants (6 or 7) produce the same primary mapping (`1%6=1%7=1`, `3%6=3%7=3`). |
| Verdict | The runtime observation is a log-parser artefact or a stale-view sender. Most likely: node3 is operating in a stale view (thinks it is in view 3 or view 10) and sending PPs that are correctly rejected by other nodes — 'rogue' in the sense of 'mismatched' but not 'accepted'. The protocol is safe. |
| Severity change | CRITICAL → LOW (observability only) |
| Concrete action | Add defensive runtime log at `tbft_replica.c:989` `pp: MAC verification failed expected_primary=%d actual_view=%lld local_view=%lld`. Add ESP_LOGI at `send_pre_prepare` next to line 2314: `send_pp: view=%lld seqno=%lld expected_primary=%d self_id=%d`. This makes 'who thinks they are primary of what view' state explicit in every PP transmission. |
| Lessons learned | The 'rogue primary' hypothesis was the most expensive false positive in the swarm investigation. It triggered a 4-finding cross-cutting re-review (FB-RP-F001 through F007) which confirmed the protocol is BFT-safe. The runtime L5 evidence was real (node3 DID send PPs in view 1) but the L5 interpretation was wrong (it inferred a code bug when the actual cause was a log-parser reading the wrong byte). Future swarm investigations should verify the parser logic against known-good data before drawing protocol-level conclusions. |

### L6-F002 — Prepare cert off-by-one (REFUTED by L7 + A9 source review)

| Field | Detail |
|-------|--------|
| S2 claim | L6-F002 (CRITICAL): "Prepare certificates complete with fewer than 2f matching prepares" — claimed `prepare_threshold=2f` in tbft_replica.c:152 is off-by-one; quorum forms with 1-2 prepares instead of 2f+1=5. |
| Refuted by | L7's root-cause synthesis (C3 contradiction resolved) + A9 source review |
| Refutation evidence | (1) `tbft_replica.c:152` comment is correct: "2f prepares (primary's PP counts)". The PP is stored as one vote via `add_pp`, then `add_prepare` / `add_my_prepare` add the remaining 2f prepares. (2) `tbft_prepared_cert_is_complete` (line 76) requires BOTH `pp_len > 0` AND prepare-cert complete (≥2f distinct prepares). (3) Total = 1 PP + 2f prepares = 2f+1, the correct PBFT threshold. (4) L2-F011 directly contradicts: "Prepare phase completes (2f+1=5) on all 3 backups for every non-OOW seqno — no quorum failures." |
| Verdict | L6 mistook a snapshot of in-progress prepare accumulation for a final state. The protocol is safe. |
| Severity change | CRITICAL → REJECTED (false positive) |
| Lessons learned | L6's protocol-trace analysis was correct in identifying anomalies but incorrect in interpreting them. The line "prepare debug: prepared=1 after a 140ms gap" was a snapshot mid-protocol, not a completed cert. Future swarm investigations should include 'is the cert complete yet?' checks before drawing quorum-formation conclusions. |

### A6-F008 — Stale-reply drain discards replies silently (REJECTED by A8)

| Field | Detail |
|-------|--------|
| S1 claim | A6-F008 (MEDIUM): "Byz_send_request's drain loop discards stale replies, may lose client-visible state" |
| Refuted by | A8 (REJECTED) |
| Refutation evidence | The drain is correct BFT behavior. `tbft_node_new_rid` increments `s_client->rid_counter` after the drain; `Byz_recv_reply` computes `expected_rid` from the post-increment counter. Stale replies for the previous rid cannot match the new rid and are correctly discarded. Re-accepting old-rid replies would re-trigger an already-aborted request. |
| Verdict | Behavior is correct. |
| Severity change | MEDIUM → REJECTED |
| Lessons learned | A6 misread the comment in the code. The code is correct; the comment accurately describes the design. |

### A7-F009 — Peer address collision (REFUTED by A8)

| Field | Detail |
|-------|--------|
| S1 claim | A7-F009 (LOW): Two different base MACs produce the same AP MAC if they differ by 1 in the last byte. |
| Refuted by | A8 (REJECTED) |
| Refutation evidence | A7's collision analysis is incorrect. Two boards with base MACs M and M+1 (last byte, no overflow) produce AP MACs M+1 and M+2 — different. Two boards with base MACs `..:fe` and `..:ff` produce AP MACs `..:ff` and `..:00` (due to overflow carry) — different. The AP MAC transformation is bijective. |
| Verdict | No real issue. |
| Severity change | LOW → REJECTED |

### A7-F022 — WDT for replica task (REFUTED by A8)

| Field | Detail |
|-------|--------|
| S1 claim | A7-F022 (LOW): "Byz_replica_run() blocks indefinitely and never calls esp_task_wdt_reset — so if the watchdog is enabled, the replica task will be reset by the watchdog." |
| Refuted by | A8 (REJECTED) |
| Refutation evidence | A7 only looked at the example file and didn't follow the call chain into the library. `Byz_replica_run` (libbyz.c:1027) calls into `tbft_replica_run` (replica.c:276) which DOES register the WDT at line 338 and reset at line 348. |
| Verdict | Replica IS subscribed to WDT. |
| Severity change | LOW → REJECTED |

---

## New Findings from Runtime

These findings are entirely new — they were missed by the static code review but the runtime analysis (or the feedback loop re-review) revealed them.

### F-NEW-01 — msg_queue drop policy is wrong for BFT (CRITICAL)

| Field | Detail |
|-------|--------|
| Source | L1-F001, L5-F005, FB-MQ-F001 (synthesis) |
| Severity | CRITICAL |
| Code | `src/tbft_transport_espnow.c:398` (consumer-boundary non-blocking enqueue) |
| Runtime evidence | 148 drops on node0, 604 cluster-wide, 75% of node0's drops are Checkpoint messages |
| Why S1 missed it | A7-F004 was identified but rated HIGH (queue depth issue). The runtime evidence (148 drops, 7/43 state-divergence events) made the impact severity clear. The feedback loop (`FB-MQ-F001`) added the policy critique: drop-on-full of fully-reassembled BFT protocol messages is the wrong policy, regardless of queue depth. |
| Concrete fix | (1) Increase `MSG_QUEUE_DEPTH` from 16 to 32. (2) Change `xQueueSend` timeout from 0 to 10ms. (3) Add the `CONFIG_TBFT_ESPNOW_MSG_QUEUE_DEPTH` Kconfig symbol that the code already references (FB-MQ-F006: currently dead override). |

### F-NEW-02 — Three uncoordinated view-change triggers (CRITICAL)

| Field | Detail |
|-------|--------|
| Source | FB-VS-F001 (synthesis from L1-F004, L5 timeline, code review) |
| Severity | CRITICAL |
| Code | `src/tbft_replica.c:387-397` (dead-primary), `:417-420` (vtimer), `:600-628` (fill timeout) |
| Runtime evidence | 6 view-changes in 2.8s; 4 of 6 are vtimer, 2 are fill-timeout |
| Why S1 missed it | A8-F001 and A9-F002 each identified one trigger; the cross-cutting view that THREE uncoordinated triggers exist with different timeouts and backoff policies was a feedback-loop synthesis. |
| Concrete fix | Unify the three triggers behind a single progress detector with shared timer and sticky backoff. Persist `r->consecutive_vc_count` and use as backoff multiplier. Reset only on real progress. |

### F-NEW-03 — vtimer period has only a 5s floor, no upper bound (CRITICAL)

| Field | Detail |
|-------|--------|
| Source | FB-VS-F004 |
| Severity | CRITICAL |
| Code | `src/tbft_libbyz.c:954-957` |
| Why S1 missed it | A3-F022 (LOW) noted the lack of upper bound but rated it as a robustness concern, not a CRITICAL misconfiguration hazard. A malicious or careless config could set `vc_timeout_ms = 1` and produce a continuous view-change storm. |
| Concrete fix | Add `if (s_replica->vtimer_period_us > 60000000LL) s_replica->vtimer_period_us = 60000000LL;` (cap at 60s). Same for stimer. |

### F-NEW-04 — Primary silently drops requests during view-change (CRITICAL)

| Field | Detail |
|-------|--------|
| Source | FB-CR-F004 (synthesis from L4-F002, A8-F005) |
| Severity | CRITICAL |
| Code | `src/tbft_replica.c:874-880` (primary's silent drop) + `src/tbft_libbyz.c:787-823` (1s+2s backoff) |
| Runtime evidence | 40% client failure rate; 35 of 35 failures are 0-reply timeouts |
| Why S1 missed it | A8-F005 (MEDIUM) noted the silent drop but rated it as a design trade-off. The runtime 40% failure rate + the counter value gaps (5→32) prove the request IS processed but the client never sees the reply — a far worse failure mode than the static review appreciated. |
| Concrete fix | (1) Primary: send a thin Status message during view-change. (2) Client: adaptive backoff. (3) Protocol: increase `TBFT_CLIENT_REPLY_TIMEOUT_MS` to 30-40s. |

### F-NEW-05 — Transport duplicate delivery via stale-slot reuse (HIGH)

| Field | Detail |
|-------|--------|
| Source | FB-CR-F001 |
| Severity | HIGH (after FB-CR-F002 confirmed app-level dedup is correct) |
| Code | `src/tbft_transport_espnow.c:382-409` |
| Runtime evidence | 11 requests receive 12-14 replies, 18 requests receive 20-22 replies (L4-F005) |
| Why S1 missed it | A7-F002 and A7-F003 covered reasm slot lifetime but not the stale-slot reuse path. The transport-level duplicate is invisible to the static review because the application-level dedup masks it. |
| Concrete fix | Clear the slot immediately after queuing the completed message, or change the slot state machine to {EMPTY, IN_PROGRESS, COMPLETED} where COMPLETED slots are skipped until the next `reasm_clear_stale` cycle. |

### F-NEW-06 — vtimer backoff reset to 1× base on most view-install paths (HIGH)

| Field | Detail |
|-------|--------|
| Source | FB-VS-F005 |
| Severity | HIGH |
| Code | `src/tbft_replica.c:1019, 1688, 2539, 2826` |
| Runtime evidence | 6-in-2.8s storm incompatible with 30s backoff |
| Why S1 missed it | The backoff exists in the vtimer path (line 3072-3082) but the reset sites are scattered; the S1 review did not connect the two. |
| Concrete fix | Persist `r->consecutive_vc_count` counter, increment on each `send_view_change`, use as backoff multiplier, reset to 0 only on real progress. |

### F-NEW-07 — Client retry uses new rid per attempt (HIGH)

| Field | Detail |
|-------|--------|
| Source | FB-CR-F005 |
| Severity | HIGH |
| Code | `src/tbft_node.c:212-216` + `src/tbft_libbyz.c:567` + `src/tbft_replica.c:887-895` |
| Runtime evidence | Counter value jumps (5→32, 46→71, 80→81, 98→125) — cluster processes each retry as a fresh request while client times out |
| Why S1 missed it | The interaction between monotonic rid generation and view-change behavior is a system-level property not visible from any single component. |
| Concrete fix | Make the client application aware of out-of-order processing. Track the last-known counter value monotonically and detect 'value went backwards'. Alternatively, add an application-level nonce to the request payload. |

### F-NEW-08 — Kconfig override symbols declared but never assigned (MEDIUM)

| Field | Detail |
|-------|--------|
| Source | FB-MQ-F006 |
| Severity | MEDIUM (build-system bug, not runtime) |
| Code | `src/tbft_transport_espnow.c:46, 52, 69` (CONFIG_TBFT_ESPNOW_MSG_QUEUE_DEPTH, CONFIG_TBFT_ESPNOW_TX_CREDITS, CONFIG_TBFT_ESPNOW_SEND_TASK_PRIORITY used in #ifndef guards but NEVER DECLARED in any Kconfig file) |
| Why S1 missed it | A7-F025 noted that the Kconfig override exists but is not documented; the actual issue is that the symbol is not declared anywhere. Operators cannot tune MSG_QUEUE_DEPTH, TX_CREDITS, or SEND_TASK_PRIORITY at compile time without modifying C source. |
| Concrete fix | Add the Kconfig symbols to a new `src/Kconfig.projbuild` or extend the existing one. |

---

## Confirmed But Underestimated

Findings that the static review identified correctly but at the wrong severity — runtime evidence revealed a more serious impact.

| S1 finding | S1 severity | Correct severity | Why underestimated | Concrete action |
|-----------|------------:|-----------------:|---------------------|-----------------|
| A1-F001 (crypto UAF) | CRITICAL | CRITICAL | None — correctly identified | Fix as in A1-F001 |
| A7-F004 (msg_queue) | HIGH | CRITICAL | Queue depth issue, not policy issue | See F-NEW-01 |
| A7-F006 (send credit) | HIGH | CRITICAL | Per-peer impact understated | See F-NEW-02 (send credit fix) |
| A8-F001 (view_installed_us=0) | HIGH | CRITICAL | Latent issue, patched in libbyz | Fix at source |
| A4-F001 (truncate new head) | MEDIUM | CRITICAL | Static review didn't connect to cascading fetch | Fix as in A4-F001 |
| A9-F002 (store_pp stuck) | MEDIUM | HIGH | Static review was correct, runtime confirmed the cascade | Fix as in A9-F002 |
| A7-F003 (reasm halving) | MEDIUM | HIGH | Static review was correct, runtime shows 8-drops-in-50ms | See F-NEW-05 |
| A7-F005 (send queue) | MEDIUM | HIGH | Static review was correct, runtime shows 14-msg VC bursts | Fix as in A7-F005 |

---

## Lessons Learned

1. **Severity must be informed by both static reasoning and runtime data.** The static review correctly identified A7-F004 (queue depth) but rated it HIGH because the static analysis alone could not quantify the impact. Runtime evidence (148 drops, 7/43 state-divergence events) made the impact severity clear.

2. **Some CRITICAL claims are log-parser artefacts.** L7's "rogue primary" claim was a 4-finding investigation that turned out to be a parser issue, not a code issue. Feedback loops must verify the parser logic against known-good data before drawing protocol-level conclusions.

3. **Cross-cutting findings need cross-cutting analysis.** A8-F001 and A9-F002 each identified one view-change trigger; the "three uncoordinated triggers" finding required stepping back and looking at all three paths together. Such findings emerge from feedback loops, not from any single agent.

4. **Static review can miss system-level interactions.** F-NEW-07 (client retry uses new rid per attempt) is invisible to any single component review — it requires understanding the monotonic rid generation + the rqueue dedup + the view-change silent drop together.

5. **Build-system bugs amplify runtime bugs.** F-NEW-08 (dead Kconfig symbols) means operators cannot tune MSG_QUEUE_DEPTH or TX_CREDITS at compile time, which means F-NEW-01 (msg_queue CRITICAL) cannot be configured away without modifying C source.

6. **Safety and availability are independent axes.** BFT safety is preserved throughout the 50-minute run (no fork, no conflicting operations), but BFT availability is broken (40% client failure rate, 5 view changes, 5/7 replicas in fetch state). The fixes needed are for availability, not safety.

7. **Runtime + feedback loop catches false positives that would otherwise consume engineering time.** L6-F002 (prepare_threshold off-by-one) was a 1-week investigation that turned out to be a parser snapshot misinterpretation. The feedback loop's C3 contradiction resolution prevented building on a wrong assumption.
