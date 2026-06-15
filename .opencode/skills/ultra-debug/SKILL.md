---
name: ultra-debug
description: >
  Two-swarm code review methodology with cross-verification and runtime log analysis.
  Use when the user wants a deep, multi-agent code review of a codebase with
  correlated runtime log analysis to prevent false positives. The review uses a
  swarm of parallel agents (one per subsystem/domain) producing structured findings,
  a cross-verification phase where agents check each other's work, a runtime log
  analysis swarm, a feedback loop where log findings inform code re-review, and
  a final aggregation producing a severity-ranked report. Works for any codebase
  with or without existing tests. For embedded/firmware projects, the runtime log
  analysis phase is especially valuable. For security-critical code, always use
  this skill before deployment.
license: MIT
compatibility: opencode >= 1.0.0
allowed-tools: Bash(mkdir *), Bash(wc -l *), Bash(ls *), Task, Read, Grep, Glob, Write, edit
argument-hint: <repo-root> [optional-swarm-count]
metadata:
  title: Ultra-Debug Code Review
  category: Code Review
  tags: [security, correctness, consensus, p2p, embedded, firmware, bft, pbft]
---

You are the orchestrator of the **Ultra-Debug** two-swarm code review methodology.
Execute this skill when the user asks for a deep code review, wants to prevent
false positives, or has runtime logs to correlate with source code.

## Methodology Overview

The Ultra-Debug review uses **three phases** across **two swarms**:

```
Phase 1 (parallel)     Phase 2 (parallel)    Phase 3 (sequential)
Swarm 1: N code        Cross-verify: N×(N    Aggregate + write
review agents           // 2) pairwise         final reports
                        verifications

Phase 4 (parallel)     Phase 5 (parallel)    Phase 6 (sequential)
Swarm 2: M log         Cross-verify log      Feedback loop + final
analysis agents         findings, L7          aggregation → final
                        synthesizes root       reports
                        causes

Phase 7 (sequential)
Feedback: L7 findings
→ targeted S1 re-review
```

- **Swarm 1** (Phase 1): N parallel domain agents, each reviewing a distinct
  subsystem in depth. N is chosen so each agent gets ~800–1,200 lines.
  Typical: N=7–12 for a ~10K–50K LOC codebase.
- **Cross-verify** (Phase 2): Each agent's findings are verified by an agent
  with overlapping domain knowledge. This is the **false-positive filter**.
- **Swarm 2** (Phase 4): M parallel log-analysis agents, each reviewing a
  distinct node/log type. M depends on the number of log files. Each agent
  produces structured findings with suspected source-file:line references.
- **Synthesis** (Phase 5): An L7-style root-cause agent integrates all
  Swarm 2 findings and maps runtime symptoms to source code locations.
- **Feedback loop** (Phase 7): Runtime findings trigger targeted re-reviews of
  contested or severe source code claims. This catches bugs the static review
  missed and upgrades/downgrades severities based on runtime evidence.

---

## Step-by-Step Execution

### Pre-flight: Analyze the codebase

Before launching agents, measure the codebase:

```bash
# Get line counts for all source files
wc -l $(find . -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.rs' | grep -v test | grep -v build | grep -v node_modules) | sort -n

# Count total lines
wc -l $(find . -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.rs' | grep -v test | grep -v build | grep -v node_modules)
```

Group related files into **domains** (typically 7–12 for manageable agent load).
Assign each agent a domain so the total lines per agent are ~800–1,200.
Large files (>2K lines) must be split across agents at natural boundaries
(function groups, phase transitions, or at fixed line ranges).

Also check for:
- Log files: `find . -name '*.log' -o -name 'data' -o -name 'logs' -type d | head -20`
- Test infrastructure: `find . -name '*test*' -o -name '*spec*' | head -20`
- CI configs: `.github/workflows/*.yml`, `Makefile`, `CMakeLists.txt`

### Swarm 1 Setup

Create the working directory:
```bash
mkdir -p .swarm/swarm1 .swarm/swarm2 .swarm/cross_verify .swarm/aggregate
```

---

### Phase 1: Launch Swarm 1 — Parallel Code Review

Launch **N agents** in parallel, one per domain. Each agent:

1. Reads every file in its assigned domain **line by line**.
2. Applies a **10-point protocol-correctness checklist** appropriate to the domain:
   - Input validation (bounds, null, integer overflow)
   - Authentication (unauthenticated state mutation risks)
   - Quorum thresholds (2f+1 vs f+1 correctness)
   - Sequence-number consistency (window watermarks, wraparound)
   - View-change safety (prepared certificates, non-forking)
   - Checkpoint safety (stable computation, GC)
   - Digest consistency (hash coverage)
   - Buffer bounds (memcpy/memset sizing)
   - Concurrency (timer callbacks, FreeRTOS/task safety)
   - Error propagation (all return codes checked)
3. Outputs a **structured JSON** to `.swarm/swarm1/A{N}_findings.json`:

```json
{
  "agent": "A1",
  "domain": "Crypto + Auth",
  "files_reviewed": ["src/crypto.c", "src/crypto.h"],
  "lines_reviewed": 937,
  "findings": [
    {
      "id": "A1-F001",
      "file": "src/crypto.c",
      "line": 234,
      "severity": "CRITICAL|HIGH|MEDIUM|LOW|INFO",
      "category": "buffer_overflow|crypto_misuse|race_condition|...",
      "finding": "Brief title",
      "evidence": "Exact code excerpt",
      "rationale": "Why this is a problem and what exploit/bug it enables",
      "suggested_fix": "How to fix"
    }
  ],
  "summary": { "critical": 0, "high": 0, "medium": 0, "low": 0, "info": 0 }
}
```

#### Standard domain splits for a PBFT/BFT consensus codebase (~10K LOC):

| Agent | Domain | Typical Files | Approx Lines |
|-------|-------|-------------|-------------|
| A1 | Crypto + Auth | principal.c/h, node.c/h | 800–950 |
| A2 | Messages + Types | message.h/c, config.h, types.h | 700–850 |
| A3 | Certificates + VC + Timers | certificate.c/h, prepared_cert.c/h, view_info.c/h, itimer.c/h | 800–950 |
| A4 | Static Memory Regions | agreement_region.c/h, checkpoint_region.c/h, special_region.c/h | 800–950 |
| A5 | State + Partition | state.c/h, partition.c/h | 900–1,100 |
| A6 | API + Config Parser | libbyz.c, esp-tinybft.h | 1,200–1,400 |
| A7 | Transport + Examples | transport_udp.c, transport_espnow.c, transport.h, examples/ | 1,500–2,000 |
| A8 | Replica Part 1 | replica.c:1–N (init, run loop, request handler) | 1,000–1,200 |
| A9 | Replica Part 2 | replica.c:N–M (normal case: pre-prepare → prepare → commit → execute) | 1,000–1,300 |
| A10 | Replica Part 3 | replica.c:M–end (view-change, fetch, key rotation, recovery) | 1,000–1,300 |

Adjust counts based on actual line totals. If a file is >2K lines, split it.
If total lines are <8K, reduce to 7 agents. If >50K, expand to 15+.

#### Domain-specific review checklists

**For crypto/auth agents (A1):**
- PSA Crypto API correctness (psa_hash_compute, psa_mac_compute, psa_asymmetric_encrypt/decrypt)
- Return codes validated before use
- Key handle lifecycle (import before use, destroy when done, no use-after-free)
- Anti-replay (timestamp monotonicity check — note if intentionally disabled)
- HMAC slot indexing (sender → slot mapping bounds)
- RSA/MAC key rotation (new_key path: decrypt, install, evict old)

**For protocol agents (A8, A9, A10):**
- Every handler validates sender_id, len, tag before state mutation
- Quorum thresholds verified: 2f+1 for commit/checkpoint/view-change, f+1 for prepare
- Seqno window: head advances on mark_stable, masking with &mask is safe for negative
- Timer callbacks do ONLY signal-and-return (no RSA, no malloc, no heavy logging)
- Event group bits consumed atomically

**For memory region agents (A4):**
- Index masking `& mask` works for seqno wraparound
- Truncate clears slots at and below new head (check new head itself is cleared)
- No buffer overflow in memcpy/memset
- Sizing grows safely with MAX_NUM_REPLICAS

**For state agents (A5):**
- CoW bitmap set before write, cleared on checkpoint
- Partition tree propagation: all children updated when one leaf changes
- Fetch protocol: Meta_data → Data chain converges
- Rollback is no-op or correct (check which)

**For transport agents (A7):**
- Queue depth adequate for fan-in/fan-out
- Drop-on-full vs blocking: which is right for BFT (drop is wrong, blocking is right)
- Fragmentation header parsing: bounds on frag_idx, frag_total
- Reassembly slot timeout: stale slots reclaimed safely

---

### Phase 2: Cross-Verification (Parallel)

Launch **N cross-verify agents**, each assigned to verify one source agent's findings.
Each verifier reads the source agent's JSON, then **reads the actual code at each cited
line**, and labels each finding:

| Label | Meaning |
|-------|---------|
| **CONFIRMED** | Issue exists, severity reasonable |
| **REJECTED** | Code is actually correct; provide counter-evidence |
| **NEEDS_CLARIFICATION** | Ambiguous; state what's needed |

**Cross-verify pairings** are chosen for domain overlap:

| Source | Verifier | Overlap Rationale |
|--------|----------|-----------------|
| A1 (Crypto) | A2 (Messages) | Authenticator sizing, digest fields |
| A2 (Messages) | A1 (Crypto) | HMAC/SIG sizes, struct alignment |
| A3 (Certs/VC) | A10 (Replica 3) | VC handlers, certificate consumption |
| A4 (Regions) | A9 (Replica 2) | Normal-case handlers use regions |
| A5 (State) | A10 (Replica 3) | Fetch protocol, checkpoint state |
| A6 (API) | A8 (Replica 1) | Init flow, public API contracts |
| A7 (Transport) | A8 (Replica 1) | Transport used in replica_run |
| A8 (Replica 1) | A6 (API) | Init, config flow |
| A9 (Replica 2) | A4 (Regions) | Region store/load in consensus |
| A10 (Replica 3) | A3 (Certs/VC) | VC uses certificates, view_info |

Output to `.swarm/cross_verify/CV_A{N}_by_A{M}.json`:

```json
{
  "verifier": "A2",
  "verified_source": "A1",
  "verifications": [
    {
      "finding_id": "A1-F001",
      "label": "CONFIRMED|REJECTED|NEEDS_CLARIFICATION",
      "code_verified_at": "src/crypto.c:234",
      "evidence": "Actual code read",
      "counter_evidence": "If REJECTED",
      "severity_adjustment": "Keep as-is|Upgrade to X|Downgrade to Y",
      "notes": "Observations from verifier's domain"
    }
  ],
  "summary": { "confirmed": 0, "rejected": 0, "needs_clarification": 0 }
}
```

---

### Phase 3: Aggregate Swarm 1

After all cross-verifications complete, merge into a single JSON per agent
applying severity adjustments. Count:
- Total findings per severity
- Confirmed vs rejected per finding
- Findings per file

Write intermediate aggregate to `.swarm/aggregate/swarm1_merged.json`.

---

### Phase 4: Launch Swarm 2 — Runtime Log Analysis

If runtime logs exist, launch **M log-analysis agents** in parallel, one per
log file or log type. If no logs exist, skip to Phase 7.

#### Log analysis domains:

| Agent | Domain | Data Source | Focus |
|-------|--------|------------|-------|
| L1 | Primary node | Largest node log | Queue overflow, send failures, bottleneck analysis |
| L2 | Backup cluster A | Medium node logs | Out-of-window patterns, checkpoint sync |
| L3 | Backup cluster B | Remaining node logs | PP storage failures, send credit exhaustion |
| L4 | Client | Client log | Retry patterns, success rate, RSA overhead |
| L5 | Global timeline | All logs merged | Cross-node event correlation, latency per phase |
| L6 | Protocol traces | All logs | Per-request trace: complete or broken chain |
| L7 | Root cause synthesis | All L1–L6 outputs | Map runtime symptoms to source file:line |

**L7 is special**: it reads all L1–L6 outputs plus the Swarm 1 findings JSONs,
synthesizes root causes, and produces:
- Structured findings with source file + line references
- Severity adjustments for Swarm 1 findings (upgrade if runtime evidence confirms)
- New findings that static review missed
- Causal chain diagrams

#### Standard log findings format:

```json
{
  "agent": "L1",
  "domain": "Primary Node Analysis",
  "log_file": "data/node0",
  "lines": 19928,
  "findings": [
    {
      "id": "L1-F001",
      "category": "queue_overflow|message_drop|protocol_violation|performance",
      "severity": "CRITICAL|HIGH|MEDIUM|LOW|INFO",
      "title": "...",
      "description": "...",
      "evidence": "Log excerpt with line numbers",
      "count": 0,
      "time_range": "t=0–t=60s",
      "suspected_root_cause": "...",
      "suspected_source_files": ["src/transport_espnow.c"],
      "suspected_lines": [398, 403]
    }
  ],
  "stats": {
    "msg_queue_drops": 0,
    "drops_by_tag": {"3": 0, "4": 0, "5": 0, "6": 0},
    "requests_received": 0,
    "commits_sent": 0
  },
  "summary": "..."
}
```

#### Root cause synthesis (L7) format:

```json
{
  "agent": "L7",
  "domain": "Root Cause Synthesis",
  "inputs_synthesized": ["L1", "L2", "L3", "L4", "L5", "L6"],
  "findings": [
    {
      "id": "L7-F001",
      "category": "root_cause",
      "severity": "CRITICAL|HIGH|MEDIUM|LOW",
      "title": "...",
      "runtime_symptom": "msg_queue full on primary",
      "log_evidence": "148 drops in first 6 minutes",
      "root_cause": "Depth=16 non-blocking queue drops fully-reassembled BFT messages",
      "suspected_file": "src/transport_espnow.c",
      "suspected_line_range": [398, 403],
      "matches_s1_finding": "A7-F004",
      "severity_change": "HIGH → CRITICAL (runtime confirms)",
      "concrete_fix": "Increase depth to 32, change xQueueSend timeout from 0 to 10ms"
    }
  ],
  "causal_chains": [
    {
      "name": "Primary queue saturation cascade",
      "steps": [
        "msg_queue depth=16 fills faster than send_task drains",
        "Checkpoint messages dropped at head of queue",
        "Backups miss checkpoints → digest mismatches → state fetches",
        "State fetches delay consensus → view-change timeout fires",
        "View-change causes 5–15s downtime per transition"
      ]
    }
  ],
  "severity_upgrades_for_s1": [
    {
      "s1_finding_id": "A7-F004",
      "current_severity": "HIGH",
      "new_severity": "CRITICAL",
      "rationale": "148 runtime drops confirmed; client 40% failure rate directly caused"
    }
  ],
  "new_findings_from_runtime": [
    {
      "id": "L7-NEW-001",
      "severity": "HIGH",
      "title": "Transport-layer duplicate reply delivery",
      "description": "espnow_recv_cb delivers same reassembled message twice",
      "suspected_file": "src/transport_espnow.c",
      "suspected_line": 0
    }
  ],
  "refuted_findings": [
    {
      "s2_finding_id": "L5-ROGUE-PRIMARY",
      "refuted_by": "FB-rogue-primary",
      "evidence": "view % num_replicas formula is correct at 14 call sites"
    }
  ],
  "summary": "..."
}
```

---

### Phase 5: Swarm 2 Cross-Verify and Synthesis

Run L7 twice if needed:
1. First pass: L7 synthesizes from L1–L6 (may run before all L1–L6 complete)
2. Corrective pass: if L7 ran before some L1–L6 files were written, re-run
   with actual data once all are available

Key: L7 must read the **actual JSON outputs** from L1–L6, not just the
symptoms described in the task prompt.

---

### Phase 6: Feedback Loop — Targeted Re-Review

Launch **4 re-review agents** for the most contested or severe runtime claims:

| Agent | Target | Key Question |
|-------|--------|--------------|
| FB-RoguePrimary | Primary selection formula | Is `view % num_replicas` correct at all call sites? |
| FB-MsgQueue | msg_queue mechanics | Is depth=16 with non-blocking really the issue? |
| FB-ViewStorm | View-change trigger logic | Are three independent triggers confirmed? |
| FB-ClientReply | Reply deduplication + 0-reply path | Is the client dedup correct at app layer? |

Each re-review agent:
1. Reads the Swarm 2 finding JSON
2. Reads the Swarm 1 findings JSON for the same area
3. Reads the **actual source code** at the suspected file:line
4. Confirms, refutes, or upgrades the finding
5. Outputs to `.swarm/swarm2/FB_{name}.json`

**Critical rule**: If a CRITICAL runtime finding is REFUTED, report it as such
with evidence. Do NOT keep CRITICAL labels on refuted findings. False
positives are worse than false negatives — the user will ignore future
reviews if this one cries wolf.

---

### Phase 7: Final Aggregation — Write Reports

Read all findings from:
- `.swarm/swarm1/A*_findings.json` (N agents)
- `.swarm/cross_verify/CV_*.json` (N×(N-1)/2 verifications)
- `.swarm/swarm2/L*_analysis.json` (M log agents)
- `.swarm/swarm2/L7_root_cause*.json` (synthesis)
- `.swarm/swarm2/FB_*.json` (feedback loop)

Produce **three markdown reports** at the repo root:

#### REPORT 1: `REVIEW_REPORT.md`

```markdown
# {Project} Code Review — Final Aggregated Report

**Date:** YYYY-MM-DD
**Scope:** {N}K LOC reviewed + {M}K lines of runtime logs
**Method:** Two-swarm review with cross-verification + feedback loop

## Executive Summary
[Stats table: severity × source]
[Top 5 most urgent findings, one paragraph each]

## CRITICAL Findings
### [ID] [Title]
- File: [file]:[line]
- Discovered by: [agent]
- Cross-verified by: [verifier]
- Runtime evidence: [yes/no/which log]
- Description: [...]
- Evidence: [code excerpt or log excerpt]
- Suggested fix: [...]

## HIGH Findings
[same format]

## Per-File Findings Table
[file | lines | crit | high | med | low | info]

## Per-Agent Statistics
[agent | domain | lines | findings | confirmed | rejected]

## Cross-Verify Matrix
[source → verifier | sent | confirmed | rejected | accuracy%]

## Severity Adjustments
[Finding | original | adjusted | reason]

## BFT/Correctness Safety Assessment
[Narrative: is the protocol fundamentally safe?]

## Recommended Fix Order
1. [Priority 1 — CRITICAL issue with fastest fix]
2. [Priority 2]
...
```

#### REPORT 2: `RUNTIME_ANALYSIS.md`

```markdown
# {Project} Runtime Log Analysis

## Run Overview
[Nodes, duration, view changes, success rate, etc.]

## Observed Issues
### [Issue Name]
- Symptom: [what the log shows]
- Count: [N occurrences]
- Time range: [when it fires]
- Root cause: [from L7 synthesis]
- Source location: [file:line]

## Per-Node Statistics
[node | log lines | drops | out-of-window | pp failures | ...]

## Phase Latency Stats
[pp_send→recv, pp_recv→commit, commit→execute, execute→reply]

## Cross-Node Anomalies
[rogue senders, asymmetric latency, one-way packet loss]

## Causal Chains
[Chain 1: step1 → step2 → ... → symptom]
[Chain 2: ...]
```

#### REPORT 3: `FINDINGS_FEEDBACK.md`

```markdown
# Cross-Swarm Feedback: Runtime → Code Review

## How Swarm 2 Informed Swarm 1

### Severity Upgrades (Runtime Confirmed)
[Finding | was | now | runtime evidence]

### Severity Downgrades (Refuted)
[Finding | was | now | refutation evidence]

### New Findings from Runtime
[Finding | severity | description | source file:line]

### Confirmed But Underestimated
[Finding | severity | new severity | reason]
```

---

## Quality Gates

The review is complete only when:

1. **All N Swarm 1 agents finished** — confirmed by checking `.swarm/swarm1/A*_findings.json`
2. **All N×(N-1)/2 cross-verifications done** — confirmed by checking `.swarm/cross_verify/CV_*.json`
3. **All M Swarm 2 log agents done** — confirmed by checking `.swarm/swarm2/L*_analysis.json`
4. **L7 synthesis done twice** (corrective pass) — confirmed by checking `.swarm/swarm2/L7_root_cause_FINAL.json`
5. **All 4 feedback agents done** — confirmed by checking `.swarm/swarm2/FB_*.json`
6. **Three reports written** to repo root — confirmed by checking `REVIEW_REPORT.md`, `RUNTIME_ANALYSIS.md`, `FINDINGS_FEEDBACK.md`

If any agent fails or produces no output, re-run it before proceeding.

---

## Agent Prompt Template

Use this template when launching each agent. Fill in the domain-specific
checklist and output path.

```
You are Agent {A1} in a code review swarm. Review the {Domain} of {Project}.

**Files to review (line-by-line):**
{files list with line counts}

**Total: {N} lines.** Read every single line.

**Focus: {Domain focus — protocol correctness / memory safety / crypto / etc.}**

**Checklist:**
1. [Domain-specific item]
2. [Domain-specific item]
... (10 items total, all mandatory)

**Output format:**
```json
{
  "agent": "{A1}",
  "domain": "{Domain}",
  "files_reviewed": [...],
  "lines_reviewed": {N},
  "findings": [
    {
      "id": "{A1-F001}",
      "file": "...",
      "line": 0,
      "severity": "CRITICAL|HIGH|MEDIUM|LOW|INFO",
      "category": "...",
      "finding": "...",
      "evidence": "...",
      "rationale": "...",
      "suggested_fix": "..."
    }
  ],
  "summary": { "critical": 0, "high": 0, "medium": 0, "low": 0, "info": 0 }
}
```

Write your output to: {WORKDIR}/.swarm/swarm1/{A1}_findings.json

Return a brief 1-paragraph summary of the most concerning findings.
```

---

## Tips for Better Results

1. **Split large files (>2K lines) at natural boundaries** — function groups,
   phase transitions, or handler boundaries. Don't split mid-function.

2. **Cross-verify pairs must have real domain overlap** — pairing A1 (crypto)
   with A7 (transport) is wrong unless crypto is used in the transport layer.

3. **L7 must read actual L1–L6 JSONs, not the task prompt** — the prompt
   describes symptoms; the JSONs have actual data. If L7 runs before L1–L6
   finish, do a corrective second pass.

4. **Refutations are as valuable as confirmations** — if a CRITICAL finding
   is refuted, report it. The user will trust the review more.

5. **Runtime logs that are larger than described** — always read the actual
   log file line counts with `wc -l` before launching agents. Logs often
   grow significantly between when you plan and when agents run.

6. **For embedded/firmware**: always include transport + timer analysis.
   These systems have unique failure modes (queue overflow, watchdog reset,
   radio backpressure) that pure code review misses.

7. **For consensus protocols (PBFT, Raft, etc.)**: always include quorum
   threshold verification as a separate checklist item. Off-by-one in f+1 vs
   2f+1 is a safety-critical bug.

8. **Agent count**: use N = ceil(total_lines / 1000), capped at 15.
   If N < 5, the codebase is too small for this methodology — use simpler review.

9. **Write findings as you go** — don't wait for all agents to finish.
   As each agent completes, note its output path and check if it's reasonable.

10. **The final report is for humans** — prioritize clarity and actionability
    over completeness. A 10-page report no one reads is worse than a 3-page
    report that drives real fixes.
