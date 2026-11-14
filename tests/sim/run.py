#!/usr/bin/env python3
"""
TinyBFT host cluster simulator: build, run scenarios, check invariants.

    python3 tests/sim/run.py                    # build + every scenario
    python3 tests/sim/run.py S1 S3              # selected scenarios
    python3 tests/sim/run.py --list
    python3 tests/sim/run.py --no-build --seed 7 --out /tmp/simruns S6a

Each node is a separate process (tests/sim/build/sim_replica, sim_client)
talking real UDP on its own loopback address 127.0.0.(11+id).  Logs of
every scenario go to <out>/<timestamp>/<scenario>/ (node<i>.log,
client.log, events.log, result.json, config + key files).

Invariants checked after the client finished and a quiescence period:
  (a) state agreement   every live replica has the same counter and state
                        digest (groups printed; "safety" if two replicas
                        disagree at the same last_exec, "lag" otherwise)
  (b) exactly-once      final counter vs the client's accepted replies:
                        ok <= counter <= ok + failed + in-flight, plus the
                        request-token analysis (no request executed twice,
                        no accepted request lost, one request per counter
                        value and per sequence number on every replica, the
                        value in each accepted reply matches the replicas)
  (c) protocol counts   view changes sent, new views installed, fetches
                        started, highest view (from logs/TBFT_STATS)
  (d) skipped seqnos    log lines where a replica passed over a sequence
                        number without executing it ("not committed ...
                        continuing", "no pre-prepare ... continuing",
                        "... skipping", "advancing window head")
plus scenario-specific liveness expectations, watchdog reports and
unexpected process exits.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXIT_RESTART, EXIT_TASK_RETURN, EXIT_ERROR_CHECK = 86, 87, 88
REBOOT_EXITS = {EXIT_RESTART, EXIT_TASK_RETURN, EXIT_ERROR_CHECK}

N_REPLICAS = 7
F = 2
CLIENT_ID = 7
IP_BASE = 11                      # node i -> 127.0.0.(11+i)

KV_RE = re.compile(r"(\w+)=(\S+)")
TIME_RE = re.compile(r"^\[\s*(-?[0-9.]+)\]")

# (d) log lines that mean "a sequence number was passed over".
SKIP_PATTERNS = collections.OrderedDict([
    ("not_committed", "not committed (skipped by view-change)"),
    ("no_pre_prepare", "execute: no pre-prepare"),
    ("digest_skip", "commit digest mismatch"),
    ("exec_skip", "skipping exec"),
    ("window_jump", "advancing window head"),
])
# The generic words the task statement lists, for a catch-all count.
SKIP_WORDS = ("not committed", "no pre-prepare", "continuing", "skipping")

EVENT_PATTERNS = collections.OrderedDict([
    ("vc_timeout", "view-change timeout"),
    ("vc_sent", "sent view-change"),
    ("nv_sent", "sent new-view"),
    ("vc_joined", "ask for a view change: joining"),
    ("vc_waits", "replicas still work in view"),
    ("nv_installed", "installed view"),
    ("nv_resent", "re-sent new-view"),
    ("view_sync", "sync view to"),
    ("status_catchup", "status view catch-up"),
    ("retransmit", "is behind (executed"),
    ("ask_missing", "asking for missing messages"),
    ("exec_wait_pp", "waiting for its pre-prepare"),
    ("exec_wait_digest", "waiting for the right one"),
    ("fetch_start", "start_fetch"),
    ("fetch_done", "state transfer complete"),
    ("fetch_timeout", "fetch timeout"),
    ("fetch_retarget", "fetching stable checkpoint"),
    ("fetch_mismatch", "does not match its certified digest"),
    ("ckpt_mismatch", "local state differs from the quorum"),
    ("req_resent", "re-sending the request"),
    ("reply_resent", "re-sending reply"),
    ("key_reply", "lacks our key"),
    ("mac_fail", "verify_mac_in: FAILED"),
    ("rq_full", "request queue full"),
])


def kv(line: str) -> dict:
    return dict(KV_RE.findall(line))


def line_time(line: str) -> float | None:
    m = TIME_RE.match(line)
    return float(m.group(1)) if m else None


# --------------------------------------------------------------------------
# Cluster control
# --------------------------------------------------------------------------

class Cluster:
    def __init__(self, args, run_dir: Path, scenario: str, rng: random.Random,
                 extra_env: dict | None = None):
        self.args = args
        self.dir = run_dir
        self.scenario = scenario
        self.rng = rng
        self.extra_env = dict(extra_env or {})
        self.bin = Path(args.build_dir)
        self.port_base = rng.randrange(20000, 60000, 10)
        self.t0_ns = time.time_ns()
        self.procs: dict[str, subprocess.Popen] = {}
        self.incarnation: dict[str, int] = collections.defaultdict(lambda: -1)
        self.expected_exit: set[str] = set()
        self.events: list[str] = []
        self.unexpected: list[dict] = []
        self.pending_reboots: list[tuple[float, str]] = []
        self.client_meta: dict = {}
        self.client_offset = 0
        self.client_state = {"ok": 0, "fail": 0, "started": 0, "done": False,
                             "inc_started": collections.Counter()}
        self.dir.mkdir(parents=True, exist_ok=True)
        self._write_config()

    # -- setup -------------------------------------------------------------
    def _write_config(self):
        a = self.args
        lines = ["counter", str(F), "3000", str(N_REPLICAS + 1), "239.0.0.1"]
        for i in range(N_REPLICAS + 1):
            host = f"node{i}" if i < N_REPLICAS else f"client{i}"
            lines.append(f"{host} 127.0.0.{IP_BASE + i} {self.port_base + i} pub{i}.key")
        lines += [str(a.vc_timeout_ms), str(a.status_timeout_ms), str(a.recovery_timeout_ms)]
        (self.dir / "config.txt").write_text("\n".join(lines) + "\n")
        for i in range(N_REPLICAS + 1):
            key = f"tinybft-sim-key node{i}\n"
            (self.dir / f"pub{i}.key").write_text(key)
            (self.dir / f"priv{i}.key").write_text(key)

    def t(self) -> float:
        return (time.time_ns() - self.t0_ns) / 1e9

    def budget(self, seconds: float) -> float:
        return seconds * self.args.budget_scale

    def event(self, msg: str):
        line = f"[{self.t():9.3f}] {msg}"
        self.events.append(line)
        with open(self.dir / "events.log", "a") as f:
            f.write(line + "\n")
        if self.args.verbose:
            print(f"    {self.scenario} {line}", flush=True)

    def _env(self, name: str) -> dict:
        env = dict(os.environ)
        env.update({
            "SIM_T0_NS": str(self.t0_ns),
            "SIM_SEED": str(self.args.seed),
            "SIM_INCARNATION": str(self.incarnation[name]),
            "SIM_LOG": self.args.log,
            "SIM_AUTH": self.args.auth,
        })
        env.update(self.extra_env)
        return env

    def _spawn(self, name: str, argv: list[str]):
        self.incarnation[name] += 1
        log = self.dir / f"{name}.log"
        with open(log, "a") as f:
            f.write(f"[{self.t():9.3f}] O (orchestrator) start {name} "
                    f"incarnation={self.incarnation[name]}\n")
        out = open(log, "ab")
        p = subprocess.Popen(argv, cwd=self.dir, env=self._env(name),
                             stdout=out, stderr=subprocess.STDOUT)
        out.close()
        self.procs[name] = p
        self.expected_exit.discard(name)

    def start_replica(self, i: int):
        name = f"node{i}"
        if self.alive(name):
            return
        self._spawn(name, [str(self.bin / "sim_replica"), str(i), "config.txt"])
        self.event(f"start node{i} (incarnation {self.incarnation[name]})")

    def start_client(self, count: int, period_ms: int, start_n: int = 1,
                     boot_delay_ms: int | None = None):
        if boot_delay_ms is None:
            boot_delay_ms = self.args.client_boot_delay_ms
        self.client_meta.setdefault("planned", 0)
        self.client_meta["planned"] = max(self.client_meta["planned"], start_n - 1 + count)
        self._spawn("client", [str(self.bin / "sim_client"), str(CLIENT_ID), "config.txt",
                               str(period_ms), str(count), "--start-n", str(start_n),
                               "--boot-delay-ms", str(boot_delay_ms)])
        self.client_state["done"] = False
        self.event(f"start client (incarnation {self.incarnation['client']}, "
                   f"requests {start_n}..{start_n + count - 1}, period {period_ms} ms, "
                   f"boot delay {boot_delay_ms} ms)")

    def start_replicas(self, order: str = "clean"):
        if order == "clean":
            # Every replica must be listening before node0 sends its first
            # New_key (node0 sends at once, node i after i*300 ms).
            for i in range(N_REPLICAS - 1, 0, -1):
                self.start_replica(i)
            self.sleep(0.15)
            self.start_replica(0)
        elif order == "id":
            for i in range(N_REPLICAS):
                self.start_replica(i)
        else:
            raise ValueError(order)

    def alive(self, name: str) -> bool:
        p = self.procs.get(name)
        return p is not None and p.poll() is None

    def kill(self, name: str, sig=signal.SIGKILL, reason: str = ""):
        p = self.procs.get(name)
        if not p or p.poll() is not None:
            return
        self.expected_exit.add(name)
        p.send_signal(sig)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        self.event(f"kill {name} with {signal.Signals(sig).name}" +
                   (f" ({reason})" if reason else ""))

    def poll(self):
        """Detect unexpected exits; reboot like a board would (after 1 s)."""
        now = self.t()
        for name, p in list(self.procs.items()):
            rc = p.poll()
            if rc is None or name in self.expected_exit:
                continue
            if name == "client" and rc == 0:
                self.expected_exit.add(name)
                self.client_state["done"] = True
                self.event("client exited normally")
                continue
            self.expected_exit.add(name)
            kind = (f"signal {signal.Signals(-rc).name}" if rc < 0 else f"exit {rc}")
            self.unexpected.append({"t": now, "node": name, "rc": rc, "kind": kind})
            self.event(f"UNEXPECTED exit of {name}: {kind}")
            if self.args.auto_reboot and (rc in REBOOT_EXITS or rc < 0):
                self.pending_reboots.append((now + 1.0, name))
        for due, name in list(self.pending_reboots):
            if now >= due:
                self.pending_reboots.remove((due, name))
                if name.startswith("node"):
                    self.event(f"reboot {name} after unexpected exit")
                    self.start_replica(int(name[4:]))

    def sleep(self, seconds: float):
        end = time.monotonic() + seconds
        while True:
            self.poll()
            left = end - time.monotonic()
            if left <= 0:
                return
            time.sleep(min(0.05, left))

    def wait_until(self, pred, timeout: float, step: float = 0.1) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.poll()
            if pred():
                return True
            time.sleep(step)
        return pred()

    # -- client progress (incremental parse of client.log) -------------------
    def client_progress(self) -> dict:
        path = self.dir / "client.log"
        if path.exists():
            with open(path, "rb") as f:
                f.seek(self.client_offset)
                data = f.read()
            end = data.rfind(b"\n")
            if end >= 0:
                self.client_offset += end + 1
                st = self.client_state
                for raw in data[:end].decode(errors="replace").splitlines():
                    if "SIM_REQ_START" in raw:
                        st["started"] += 1
                        st["inc_started"][self.incarnation["client"]] += 1
                    elif "SIM_REQ_END" in raw:
                        if kv(raw).get("ok") == "1":
                            st["ok"] += 1
                        else:
                            st["fail"] += 1
                    elif "SIM_CLIENT_DONE" in raw:
                        st["done"] = True
        return self.client_state

    def wait_client(self, budget_s: float) -> bool:
        """Wait for the client to finish; kill it when the budget runs out."""
        ok = self.wait_until(lambda: self.client_progress()["done"] or
                             not self.alive("client"), budget_s, step=0.2)
        self.client_progress()
        if not self.client_progress()["done"]:
            self.client_meta["budget_exceeded"] = True
            self.kill("client", signal.SIGTERM, reason=f"time budget {budget_s:.0f} s exhausted")
            self.client_progress()
            return False
        return ok

    def stop_all(self):
        """SIGTERM replicas (they print a final SIM_REPLICA line), then kill."""
        for name in list(self.procs):
            if self.alive(name):
                self.expected_exit.add(name)
                self.procs[name].send_signal(signal.SIGTERM)
        deadline = time.monotonic() + 5
        for name, p in self.procs.items():
            try:
                p.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                self.event(f"{name} did not exit on SIGTERM; killed")
        self.event("all processes stopped")

    def live_at_stop(self) -> set[str]:
        return {n for n, p in self.procs.items() if p.poll() is None}


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------

SCENARIOS = collections.OrderedDict()


def scenario(name, title, env=None):
    def deco(fn):
        SCENARIOS[name] = {"fn": fn, "title": title, "env": env or {}}
        return fn
    return deco


@scenario("S1", "normal: all start together (clean key exchange), 40 req @ 500 ms")
def s1(c: Cluster):
    c.start_replicas("clean")
    c.start_client(40, 500)
    c.wait_client(c.budget(120))
    c.expect = {"all_ok": True}


@scenario("S1r", "boot race: all start together in id order (node0 first), 40 req @ 500 ms")
def s1r(c: Cluster):
    c.start_replicas("id")
    c.start_client(40, 500)
    c.wait_client(c.budget(170))
    c.expect = {"eventual": True}


def staggered(c: Cluster, client_first: bool):
    offsets = {i: round(c.rng.uniform(0, 30), 2) for i in range(N_REPLICAS)}
    c.event("replica start offsets: " + " ".join(f"node{i}@{o:.2f}s" for i, o in
                                                 sorted(offsets.items(), key=lambda x: x[1])))
    start = time.monotonic()
    if client_first:
        c.start_client(40, 500)
    for i, off in sorted(offsets.items(), key=lambda x: x[1]):
        c.sleep(max(0.0, off - (time.monotonic() - start)))
        c.start_replica(i)
    if not client_first:
        c.sleep(max(0.0, 32 - (time.monotonic() - start)))
        c.start_client(40, 500)
    c.wait_client(c.budget(175) - (time.monotonic() - start))
    c.expect = {"eventual": True}


@scenario("S2a", "staggered boot 0-30 s, client starts first, 40 req @ 500 ms")
def s2a(c: Cluster):
    staggered(c, client_first=True)


@scenario("S2b", "staggered boot 0-30 s, client starts after the last replica, 40 req @ 500 ms")
def s2b(c: Cluster):
    staggered(c, client_first=False)


@scenario("S3", "primary crash: kill node0 after 10 accepted requests, 60 req @ 500 ms")
def s3(c: Cluster):
    c.start_replicas("clean")
    c.start_client(60, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 10, c.budget(90)):
        c.kill("node0", signal.SIGKILL, reason="primary crash")
        c.client_meta["crash_at_ok"] = c.client_progress()["ok"]
        c.client_meta["crash_at_started"] = c.client_progress()["started"]
        c.client_meta["crash_t"] = c.t()
    c.wait_client(c.budget(170) - c.t())
    c.expect = {"progress_after_crash": True, "eventual": True}


@scenario("S3b", "two crashes: kill node0 and node1 (primaries of views 0 and 1) after 10 accepted, 60 req @ 500 ms")
def s3b(c: Cluster):
    c.start_replicas("clean")
    c.start_client(60, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 10, c.budget(90)):
        c.kill("node0", signal.SIGKILL, reason="primary crash")
        c.kill("node1", signal.SIGKILL, reason="next primary crash")
        c.client_meta["crash_at_ok"] = c.client_progress()["ok"]
        c.client_meta["crash_at_started"] = c.client_progress()["started"]
        c.client_meta["crash_t"] = c.t()
    c.wait_client(c.budget(170) - c.t())
    c.expect = {"progress_after_crash": True, "eventual": True}


@scenario("S3r", "primary crash and return: kill node0 after 10 accepted, restart it 15 s later, 80 req @ 500 ms")
def s3r(c: Cluster):
    c.start_replicas("clean")
    c.start_client(80, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 10, c.budget(90)):
        c.kill("node0", signal.SIGKILL, reason="primary crash")
        c.client_meta["crash_at_ok"] = c.client_progress()["ok"]
        c.client_meta["crash_at_started"] = c.client_progress()["started"]
        c.client_meta["crash_t"] = c.t()
        c.sleep(15)
        c.start_replica(0)
        c.client_meta["reboot_t"] = c.t()
    c.wait_client(c.budget(170) - c.t())
    c.quiesce_s = 15
    c.expect = {"catch_up": "node0", "progress_after_crash": True, "eventual": True}


@scenario("S4", "replica reboot: kill node3 after 15 accepted, restart 20 s later, 120 req @ 500 ms")
def s4(c: Cluster):
    c.start_replicas("clean")
    c.start_client(120, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 15, c.budget(90)):
        c.kill("node3", signal.SIGKILL, reason="replica crash")
        c.sleep(20)
        c.start_replica(3)
        c.client_meta["reboot_t"] = c.t()
    c.wait_client(c.budget(165) - c.t())
    c.quiesce_s = 15
    c.expect = {"catch_up": "node3", "all_ok": True}


@scenario("S5", "client reboot: kill the client after 15 accepted, restart it 3 s later, 40 req @ 500 ms")
def s5(c: Cluster):
    c.start_replicas("clean")
    total = 40
    c.start_client(total, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 15, c.budget(90)):
        c.kill("client", signal.SIGKILL, reason="client crash")
        prog = c.client_progress()
        started = prog["started"]
        c.client_meta["first_incarnation_started"] = started
        c.sleep(3)
        c.start_client(total - started, 500, start_n=started + 1)
        c.client_meta["restart_t"] = c.t()
    c.wait_client(c.budget(170) - c.t())
    c.expect = {"post_restart_ok": True}


@scenario("S5p", "client reboot, then primary crash: client restarted after 15 accepted, "
                 "node0 killed after 3 more, 40 req @ 500 ms")
def s5p(c: Cluster):
    c.start_replicas("clean")
    total = 40
    c.start_client(total, 500)
    if c.wait_until(lambda: c.client_progress()["ok"] >= 15, c.budget(90)):
        c.kill("client", signal.SIGKILL, reason="client crash")
        started = c.client_progress()["started"]
        c.client_meta["first_incarnation_started"] = started
        c.sleep(3)
        c.start_client(total - started, 500, start_n=started + 1)
        c.client_meta["restart_t"] = c.t()
        ok_before = c.client_progress()["ok"]
        if c.wait_until(lambda: c.client_progress()["ok"] >= ok_before + 3, c.budget(60)):
            c.kill("node0", signal.SIGKILL, reason="primary crash after client reboot")
            c.client_meta["crash_t"] = c.t()
    c.wait_client(c.budget(170) - c.t())
    c.expect = {"progress_after_crash": True, "eventual": True}


@scenario("S6a", "loss 5 % on every datagram (all nodes), 40 req @ 500 ms",
          env={"SIM_LOSS": "0.05"})
def s6a(c: Cluster):
    c.start_replicas("clean")
    c.start_client(40, 500)
    c.wait_client(c.budget(170))
    c.expect = {"min_ok_frac": 0.9}


@scenario("S6b", "loss 10 % on every datagram (all nodes), 40 req @ 500 ms",
          env={"SIM_LOSS": "0.10"})
def s6b(c: Cluster):
    c.start_replicas("clean")
    c.start_client(40, 500)
    c.wait_client(c.budget(170))
    c.expect = {"min_ok_frac": 0.9}


# --------------------------------------------------------------------------
# Log analysis
# --------------------------------------------------------------------------

def parse_replica_log(path: Path) -> dict:
    res = {"incarnations": [], "execs": [], "skips": collections.Counter(),
           "skip_samples": [], "skip_words": collections.Counter(),
           "events": collections.Counter(), "wdt": [], "restarts": 0,
           "sanitizer": [], "tbft_stats": [], "series": []}
    if not path.exists():
        return res
    inc = None
    for line in path.read_text(errors="replace").splitlines():
        if "SIM_BOOT" in line:
            inc = {"boot": kv(line), "last": None, "final": None}
            res["incarnations"].append(inc)
            continue
        if "SIM_REPLICA" in line:
            d = kv(line)
            if inc is not None:
                inc["last"] = d
                if d.get("final") == "1":
                    inc["final"] = d
            res["series"].append((line_time(line), len(res["incarnations"]) - 1, d))
            continue
        if "SIM_EXEC" in line:
            d = kv(line)
            d["_inc"] = len(res["incarnations"]) - 1
            d["_t"] = line_time(line)
            res["execs"].append(d)
            continue
        if "TBFT_STATS" in line:
            res["tbft_stats"].append(kv(line))
        if "SIM_WDT" in line:
            res["wdt"].append(line)
        if "SIM_RESTART" in line or "should not return" in line or "ESP_ERROR_CHECK failed" in line:
            res["restarts"] += 1
        if "AddressSanitizer" in line or "runtime error:" in line or "LeakSanitizer" in line:
            res["sanitizer"].append(line)
        for key, pat in SKIP_PATTERNS.items():
            if pat in line:
                res["skips"][key] += 1
                if len(res["skip_samples"]) < 5:
                    res["skip_samples"].append(line.strip())
        if " W (" in line or " I (" in line or " E (" in line:
            for w in SKIP_WORDS:
                if w in line:
                    res["skip_words"][w] += 1
        for key, pat in EVENT_PATTERNS.items():
            if pat in line:
                res["events"][key] += 1
    return res


def parse_client_log(path: Path) -> dict:
    res = {"starts": {}, "ends": {}, "done": [], "inc": -1, "order": []}
    if not path.exists():
        return res
    for line in path.read_text(errors="replace").splitlines():
        if "SIM_BOOT" in line:
            res["inc"] += 1
        elif "SIM_REQ_START" in line:
            d = kv(line)
            d["_t"] = line_time(line)
            d["_inc"] = res["inc"]
            res["starts"][d["token"]] = d
            res["order"].append(d["token"])
        elif "SIM_REQ_END" in line:
            d = kv(line)
            d["_t"] = line_time(line)
            d["_inc"] = res["inc"]
            res["ends"][d["token"]] = d
        elif "SIM_CLIENT_DONE" in line:
            res["done"].append(kv(line))
    return res


def analyse(c: Cluster, live: set[str]) -> dict:
    reps = {i: parse_replica_log(c.dir / f"node{i}.log") for i in range(N_REPLICAS)}
    cli = parse_client_log(c.dir / "client.log")
    r: dict = {"scenario": c.scenario, "checks": {}, "notes": []}

    # ---- client numbers
    ok_tokens = [t for t, e in cli["ends"].items() if e.get("ok") == "1"]
    fail_tokens = [t for t, e in cli["ends"].items() if e.get("ok") != "1"]
    inflight = [t for t in cli["starts"] if t not in cli["ends"]]
    planned = c.client_meta.get("planned", 0)
    not_sent = max(0, planned - len(cli["starts"]))
    r["client"] = {"ok": len(ok_tokens), "fail": len(fail_tokens),
                   "inflight": len(inflight), "not_sent": not_sent,
                   "planned": planned,
                   "budget_exceeded": bool(c.client_meta.get("budget_exceeded"))}
    lat = [int(e["lat_ms"]) for e in cli["ends"].values() if e.get("ok") == "1"]
    if lat:
        r["client"]["lat_ms_avg"] = round(sum(lat) / len(lat), 1)
        r["client"]["lat_ms_max"] = max(lat)

    # ---- (a) state agreement over live replicas
    finals = {}
    for i in range(N_REPLICAS):
        if f"node{i}" not in live:
            continue
        incs = reps[i]["incarnations"]
        if not incs:
            continue
        last = incs[-1]["final"] or incs[-1]["last"]
        if last:
            finals[i] = last
            if not incs[-1]["final"]:
                r["notes"].append(f"node{i}: no final status line (hung?)")
    groups = collections.defaultdict(list)
    for i, d in finals.items():
        groups[(int(d.get("counter", -1)), d.get("state_digest", "?"))].append(i)
    group_list = sorted(groups.items(), key=lambda kvp: -len(kvp[1]))
    safety = False
    by_exec = collections.defaultdict(set)
    for i, d in finals.items():
        by_exec[d.get("last_exec")].add((d.get("counter"), d.get("state_digest")))
    for le, states in by_exec.items():
        if len(states) > 1:
            safety = True
    if len(groups) == 1 and len(finals) > 0:
        a_status = "PASS"
    elif not finals:
        a_status = "FAIL"
    else:
        a_status = "FAIL(safety)" if safety else "FAIL(lag)"
    r["checks"]["a"] = {
        "status": a_status,
        "groups": [{"counter": k[0], "digest": k[1][:12],
                    "last_exec": sorted({finals[i].get("last_exec") for i in v}),
                    "nodes": v} for k, v in group_list],
        "live": sorted(finals),
    }
    majority_counter = group_list[0][0][0] if group_list else None
    max_counter = max((int(d.get("counter", -1)) for d in finals.values()), default=None)

    # ---- (b) exactly-once, ordering, reply correctness
    token_values = collections.defaultdict(set)      # token -> {value}
    value_tokens = collections.defaultdict(set)      # value -> {token}
    seq_tokens = collections.defaultdict(set)        # seqno -> {token}
    per_replica_dup = []
    for i, rep in reps.items():
        seen = collections.Counter()
        for e in rep["execs"]:
            tok, val, seq = e.get("token"), int(e.get("value", -1)), int(e.get("seqno", -1))
            token_values[tok].add(val)
            value_tokens[val].add(tok)
            seq_tokens[seq].add(tok)
            seen[(e["_inc"], tok)] += 1
        for (inc, tok), n in seen.items():
            if n > 1:
                per_replica_dup.append(f"node{i}#{inc} executed {tok} {n}x")
    dup_tokens = sorted(t for t, v in token_values.items() if len(v) > 1)
    value_conflicts = sorted(v for v, t in value_tokens.items() if len(t) > 1)
    seq_conflicts = sorted(s for s, t in seq_tokens.items() if len(t) > 1)
    executed = set(token_values)
    lost = sorted(t for t in ok_tokens if t not in executed)
    wrong_reply = []
    for t in ok_tokens:
        v = int(cli["ends"][t].get("value", -1))
        if t in token_values and v not in token_values[t]:
            wrong_reply.append(f"{t}: reply {v}, executed at {sorted(token_values[t])}")
    exec_failed = sorted(t for t in fail_tokens + inflight if t in executed)
    ok_n, fail_n, infl_n = len(ok_tokens), len(fail_tokens), len(inflight)
    counter_ok = (majority_counter is not None and
                  ok_n <= majority_counter <= ok_n + fail_n + infl_n)
    count_match = (majority_counter == len(executed)) if majority_counter is not None else False
    b_fail = []
    if not counter_ok:
        b_fail.append(f"counter {majority_counter} not in [ok={ok_n}, ok+fail+inflight={ok_n + fail_n + infl_n}]")
    if dup_tokens or per_replica_dup:
        b_fail.append(f"duplicate execution of {len(dup_tokens) or len(per_replica_dup)} request(s)")
    if lost:
        b_fail.append(f"{len(lost)} accepted request(s) never executed")
    if value_conflicts:
        b_fail.append(f"{len(value_conflicts)} counter value(s) produced by different requests")
    if seq_conflicts:
        b_fail.append(f"{len(seq_conflicts)} seqno(s) executed with different requests")
    if wrong_reply:
        b_fail.append(f"{len(wrong_reply)} reply value(s) differ from execution")
    if not count_match and majority_counter is not None:
        b_fail.append(f"majority counter {majority_counter} != distinct executed requests {len(executed)}")
    r["checks"]["b"] = {
        "status": "PASS" if not b_fail else "FAIL",
        "why": b_fail,
        "majority_counter": majority_counter, "max_counter": max_counter,
        "executed_distinct": len(executed),
        "executed_but_not_acknowledged": len(exec_failed),
        "dup_tokens": dup_tokens[:10], "per_replica_dup": per_replica_dup[:10],
        "lost": lost[:10], "value_conflicts": value_conflicts[:10],
        "seq_conflicts": seq_conflicts[:10], "wrong_reply": wrong_reply[:10],
    }

    # ---- (c) protocol counts
    ev = collections.Counter()
    for rep in reps.values():
        ev.update(rep["events"])
    max_view = max((int(d.get("view", 0)) for rep in reps.values()
                    for (_, _, d) in rep["series"][-3:]), default=0)
    stat_sum = collections.Counter()
    for rep in reps.values():
        for inc in rep["incarnations"]:
            d = inc["final"] or inc["last"]
            if not d:
                continue
            for k in ("vc_sent", "new_view", "fetch", "commit_resent"):
                stat_sum[k] += int(d.get(k, 0))
    r["checks"]["c"] = {
        "vc_sent": stat_sum["vc_sent"], "new_view": stat_sum["new_view"],
        "fetch": stat_sum["fetch"], "commit_resent": stat_sum["commit_resent"],
        "max_view": max_view, "log_events": dict(ev),
    }

    # ---- (d) skipped sequence numbers
    skips = collections.Counter()
    words = collections.Counter()
    samples = []
    for i, rep in reps.items():
        skips.update(rep["skips"])
        words.update(rep["skip_words"])
        samples += [f"node{i}: {s}" for s in rep["skip_samples"]]
    total_skips = sum(skips.values())
    r["checks"]["d"] = {"status": "PASS" if total_skips == 0 else "FLAG",
                        "count": total_skips, "by_kind": dict(skips),
                        "keyword_lines": dict(words), "samples": samples[:8]}

    # ---- other health
    wdt = sum(len(rep["wdt"]) for rep in reps.values())
    san = [s for rep in reps.values() for s in rep["sanitizer"]]
    r["health"] = {"unexpected_exits": c.unexpected, "watchdog": wdt,
                   "sanitizer": san[:5]}

    # ---- scenario expectations (liveness)
    exp = getattr(c, "expect", {})
    live_checks = {}
    if exp.get("all_ok"):
        live_checks["all requests accepted"] = (ok_n == planned and fail_n == 0 and infl_n == 0)
    if exp.get("eventual"):
        ends_sorted = sorted(cli["ends"].values(), key=lambda e: e["_t"] or 0)
        last = ends_sorted[-5:]
        live_checks["steady state reached (last 5 requests accepted)"] = (
            len(last) == 5 and all(e.get("ok") == "1" for e in last))
        first_ok = next((e["_t"] for e in ends_sorted if e.get("ok") == "1"), None)
        r["client"]["first_ok_t"] = first_ok
    if "min_ok_frac" in exp:
        frac = exp["min_ok_frac"]
        live_checks[f">= {frac:.0%} of planned requests accepted"] = (
            planned > 0 and ok_n >= frac * planned)
    if exp.get("progress_after_crash"):
        t_crash = c.client_meta.get("crash_t")
        after = [e for e in cli["ends"].values()
                 if e.get("ok") == "1" and t_crash is not None and e["_t"] > t_crash]
        live_checks["requests accepted after the crash"] = len(after) > 0
        r["client"]["ok_after_crash"] = len(after)
        r["client"]["fail_after_crash"] = sum(
            1 for e in cli["ends"].values()
            if e.get("ok") != "1" and t_crash is not None and e["_t"] > t_crash)
    if exp.get("post_restart_ok"):
        post = [e for e in cli["ends"].values() if e["_inc"] >= 1]
        live_checks["all requests after the client restart accepted"] = (
            len(post) > 0 and all(e.get("ok") == "1" for e in post))
        r["client"]["post_restart"] = (sum(e.get("ok") == "1" for e in post), len(post))
    if exp.get("catch_up"):
        node = int(exp["catch_up"][4:])
        live_checks[f"{exp['catch_up']} caught up"] = (
            node in finals and majority_counter is not None and
            int(finals[node].get("counter", -1)) == majority_counter)
        series = [(t, d) for (t, inc, d) in reps[node]["series"] if inc >= 1]
        t_reboot = c.client_meta.get("reboot_t")
        for t, d in series:
            if majority_counter is not None and int(d.get("counter", -1)) >= majority_counter:
                r["notes"].append(f"node{node} reached counter {majority_counter} "
                                  f"{t - t_reboot:.1f} s after its restart" if t_reboot else "")
                break
        keys = [(t, int(d.get("keys", 0))) for t, d in series]
        for t, k in keys:
            if k >= 2 * F:
                r["notes"].append(f"node{node} had 2f={2 * F} peer keys "
                                  f"{t - t_reboot:.1f} s after restart" if t_reboot else "")
                break
    r["checks"]["live"] = {"status": ("PASS" if all(live_checks.values()) else "FAIL")
                           if live_checks else "-", "items": live_checks}
    return r


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def short_groups(a: dict) -> str:
    parts = []
    for g in a["groups"]:
        nodes = ",".join(str(n) for n in g["nodes"])
        parts.append(f"{g['counter']}@[{nodes}]")
    return " ".join(parts)


def print_table(results: list[dict]):
    hdr = (f"{'scenario':8} {'time':>6} {'client ok/fail/infl':>20}  {'(a) state':10} "
           f"{'(b) exactly-once':16} {'(c) vc/nv/fetch/view':>21}  {'(d) skips':9} {'liveness':8}")
    print()
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        cl = r["client"]
        cc = r["checks"]["c"]
        cli_s = f"{cl['ok']}/{cl['fail']}/{cl['inflight']}" + (
            f" (+{cl['not_sent']} unsent)" if cl["not_sent"] else "")
        print(f"{r['scenario']:8} {r['runtime_s']:>5.0f}s {cli_s:>20}  "
              f"{r['checks']['a']['status']:10} {r['checks']['b']['status']:16} "
              f"{cc['vc_sent']:>5}/{cc['new_view']}/{cc['fetch']}/{cc['max_view']:<5}      "
              f"{r['checks']['d']['status'] + '(' + str(r['checks']['d']['count']) + ')':9} "
              f"{r['checks']['live']['status']:8}")
    print()
    for r in results:
        print(f"== {r['scenario']}: {r['title']}")
        a = r["checks"]["a"]
        print(f"   (a) {a['status']}: counter@[nodes] {short_groups(a)}")
        b = r["checks"]["b"]
        print(f"   (b) {b['status']}: majority counter={b['majority_counter']} "
              f"distinct executed={b['executed_distinct']} client ok={r['client']['ok']} "
              f"fail={r['client']['fail']} inflight={r['client']['inflight']} "
              f"executed-but-unacknowledged={b['executed_but_not_acknowledged']}")
        for w in b["why"]:
            print(f"       - {w}")
        for key in ("dup_tokens", "per_replica_dup", "lost", "value_conflicts",
                    "seq_conflicts", "wrong_reply"):
            if b[key]:
                print(f"       {key}: {b[key][:5]}")
        c = r["checks"]["c"]
        print(f"   (c) vc_sent={c['vc_sent']} new_view={c['new_view']} fetch={c['fetch']} "
              f"commit_resent={c['commit_resent']} max_view={c['max_view']} "
              f"events={dict(sorted(c['log_events'].items()))}")
        d = r["checks"]["d"]
        print(f"   (d) {d['status']}: {d['count']} skip line(s) {d['by_kind']}")
        for s in d["samples"][:3]:
            print(f"       {s[:160]}")
        lv = r["checks"]["live"]
        if lv["items"]:
            print(f"   liveness {lv['status']}: " +
                  ", ".join(f"{k}={'yes' if v else 'NO'}" for k, v in lv["items"].items()))
        h = r["health"]
        if h["unexpected_exits"] or h["watchdog"] or h["sanitizer"]:
            print(f"   health: unexpected exits={[(u['node'], u['kind']) for u in h['unexpected_exits']]} "
                  f"watchdog={h['watchdog']} sanitizer={h['sanitizer'][:2]}")
        cl = r["client"]
        extra = {k: v for k, v in cl.items() if k not in ("ok", "fail", "inflight", "not_sent", "planned")}
        if extra:
            print(f"   client: {extra}")
        for n in r["notes"]:
            if n:
                print(f"   note: {n}")
        print(f"   logs: {r['dir']}")
    print()


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def build_dir_name(args) -> str:
    name = "build-san" if args.san else "build"
    if args.client_timeout_ms != 20000:
        name += f"-ct{args.client_timeout_ms}"
    return name


def build(args):
    cmd = ["make", "-C", str(HERE), f"-j{os.cpu_count() or 4}",
           f"BUILD={build_dir_name(args)}"]
    if args.san:
        cmd.append(f"SAN={args.san}")
    if args.client_timeout_ms != 20000:
        cmd.append(f"EXTRA_CFLAGS=-DCONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS={args.client_timeout_ms}")
    print("building:", " ".join(cmd), flush=True)
    res = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write("".join(l + "\n" for l in res.stdout.splitlines() if "warning" in l or "error" in l))
    sys.stdout.write(res.stderr)
    if res.returncode != 0:
        sys.exit("build failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="scenario names (default: all)")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--san", default="", help="build/run with sanitizers, e.g. address,undefined")
    ap.add_argument("--out", default=str(HERE / "runs"), help="run directory root")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--repeat", type=int, default=1, help="run each scenario N times (seed+k)")
    ap.add_argument("--log", default="WARN",
                    help="SIM_LOG for every node, e.g. INFO or WARN,tbft_replica=INFO")
    ap.add_argument("--auth", default="strict", choices=["strict", "lax"],
                    help="stub principal verification mode (SIM_AUTH)")
    ap.add_argument("--vc-timeout-ms", type=int, default=3000)
    ap.add_argument("--status-timeout-ms", type=int, default=500)
    ap.add_argument("--recovery-timeout-ms", type=int, default=1000)
    ap.add_argument("--hw-timeouts", action="store_true",
                    help="use gen_configs.sh's timeouts: view change 30000, status 5000, "
                         "recovery 10000 ms (slow)")
    ap.add_argument("--client-timeout-ms", type=int, default=20000,
                    help="CONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS (compile time; a value other "
                         "than 20000 builds into build-ct<N>/)")
    ap.add_argument("--budget-scale", type=float, default=1.0,
                    help="multiply every scenario's time budget (e.g. 3 with --hw-timeouts)")
    ap.add_argument("--client-boot-delay-ms", type=int, default=5000,
                    help="client delay before Byz_init_client (example: 5000)")
    ap.add_argument("--quiesce-s", type=float, default=10)
    ap.add_argument("--no-auto-reboot", dest="auto_reboot", action="store_false",
                    help="do not restart a node that exits unexpectedly")
    ap.add_argument("--env", action="append", default=[],
                    help="extra NAME=VALUE for every node (e.g. SIM_LOSS=0.02)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--reanalyze", nargs="+", metavar="DIR",
                    help="re-run the checks on existing scenario directories (with meta.json)")
    args = ap.parse_args()

    if args.list:
        for k, v in SCENARIOS.items():
            print(f"{k:5} {v['title']}")
        return
    if args.reanalyze:
        results = []
        for d in args.reanalyze:
            d = Path(d)
            meta = json.loads((d / "meta.json").read_text())
            c = argparse.Namespace(dir=d, scenario=meta["scenario"],
                                   client_meta=meta["client_meta"],
                                   unexpected=meta["unexpected"], expect=meta["expect"])
            r = analyse(c, set(meta["live"]))
            r.update(title=meta["title"], runtime_s=meta["runtime_s"], dir=str(d),
                     seed=meta["seed"])
            results.append(r)
        print_table(results)
        return
    names = args.scenarios or list(SCENARIOS)
    for n in names:
        if n not in SCENARIOS:
            sys.exit(f"unknown scenario {n} (try --list)")
    if args.hw_timeouts:
        args.vc_timeout_ms, args.status_timeout_ms, args.recovery_timeout_ms = 30000, 5000, 10000
    args.build_dir = str(HERE / build_dir_name(args))
    if not args.no_build:
        build(args)
    extra_env = dict(e.split("=", 1) for e in args.env)

    root = Path(args.out) / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True, exist_ok=True)
    print(f"run directory: {root}", flush=True)
    results = []
    clusters = []
    try:
        for rep in range(args.repeat):
            for name in names:
                sc = SCENARIOS[name]
                seed = args.seed + rep
                label = name if args.repeat == 1 else f"{name}.{rep}"
                rng = random.Random(f"{seed}-{name}")
                env = dict(sc["env"])
                env.update(extra_env)
                local = argparse.Namespace(**vars(args))
                local.seed = seed
                c = Cluster(local, root / label, label, rng, env)
                clusters.append(c)
                print(f"--- {label}: {sc['title']} (seed {seed}, ports {c.port_base}..)",
                      flush=True)
                t_start = time.monotonic()
                c.quiesce_s = args.quiesce_s
                try:
                    sc["fn"](c)
                    c.event(f"workload finished; quiescence {c.quiesce_s:.0f} s")
                    c.sleep(c.quiesce_s)
                    live = c.live_at_stop()
                finally:
                    c.stop_all()
                runtime = time.monotonic() - t_start
                meta = {"scenario": label, "title": sc["title"], "seed": seed,
                        "runtime_s": runtime, "live": sorted(live),
                        "client_meta": c.client_meta, "unexpected": c.unexpected,
                        "expect": getattr(c, "expect", {}),
                        "timeouts_ms": [args.vc_timeout_ms, args.status_timeout_ms,
                                        args.recovery_timeout_ms, args.client_timeout_ms],
                        "auth": args.auth, "env": env}
                (c.dir / "meta.json").write_text(json.dumps(meta, indent=2, default=str))
                r = analyse(c, live)
                r["title"] = sc["title"]
                r["runtime_s"] = runtime
                r["dir"] = str(c.dir)
                r["seed"] = seed
                (c.dir / "result.json").write_text(json.dumps(r, indent=2, default=str))
                results.append(r)
                cl = r["client"]
                print(f"    done in {runtime:.0f} s: client ok={cl['ok']} fail={cl['fail']} "
                      f"inflight={cl['inflight']}; a={r['checks']['a']['status']} "
                      f"b={r['checks']['b']['status']} d={r['checks']['d']['status']} "
                      f"live={r['checks']['live']['status']}", flush=True)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
    finally:
        for c in clusters:
            for p in c.procs.values():
                if p.poll() is None:
                    p.kill()
    (root / "summary.json").write_text(json.dumps(results, indent=2, default=str))
    print_table(results)
    print(f"summary: {root / 'summary.json'}")


if __name__ == "__main__":
    main()
