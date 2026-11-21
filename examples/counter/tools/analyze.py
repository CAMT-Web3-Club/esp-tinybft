#!/usr/bin/env python3
"""Summarise measured runs and pick the best configuration per transport.

Optional helper: the firmware and menuconfig do not need it.

Layout (one directory per run; you choose the configuration name, repeats are
named <name>-r1, -r2, ...):
  runs/<name>-r1/client7.log      serial logs, one file per board; the file
  runs/<name>-r1/node0.log ...    stem must match the node label in energy.csv
  runs/<name>-r1/energy.csv       node,joules[,seconds]  OR  node,avg_ma,volts,seconds
                                  (seconds is needed for the incremental column
                                  and for the window check)
  runs/<name>-base/               optional baseline: same image, client OFF
                                  (energy.csv only; replica logs optional)

The settings of each configuration (transport, rate, power saving, timeouts,
...) are read from the BANNER line the boards print at boot, so keep the boot
output in the logs.

The client log is required: it gives success, latency and, from the view of
the replies it accepted, every view change of the run.  The replica logs add
state fetches, re-sent messages and transport drops; without them a run with
a state fetch cannot be excluded (a warning says so).  Keep logging for at
least 60 s after the last request: the replicas print TBFT_STATS every 60 s.

A run lasts from power-on until the batteries are empty, and all of it is
counted: boot, key exchange, the requests that fail while the cluster comes up,
and the last ones before a board browns out.  Success counts every request the
client sent since it booted (counters summed over client reboots), latency
covers every success, the first one after boot included, and energy.csv must
cover the same span (power-on to the end).  The warm-up is reported, not
removed: "first OK s" is the client's uptime at its first success and
"warm-up fails" the requests that failed before it.

The client may instead stay powered between runs while only the replicas run
on batteries: start its log when the batteries go in.  A client log that
starts with CLIENT_STATS and no BANNER before it is taken as "already
running": its first CLIENT_STATS (and TBFT_STATS) are the baseline, the run is
counted from there, and "first OK s" is measured from that line.

The client runs one of two loads (BANNER load=): back-to-back (b2b), where
the next request starts as soon as the previous one completes, or one request
every 10 s (p10).  Never mix the two in one comparison: name the runs apart
(<preset> and <preset>_p10).  "success (sent)" is ok / (ok + fail), the
quality of the link and the protocol.  "slots" is ok / (ok + fail + missed):
with p10 a slot is "missed" when the previous request was still running at
the next period, so a higher latency lowers "slots" while "success (sent)"
stays at 100 %; with b2b nothing is missed and the two are equal.  Measure
the SAME boards in every run: efficacy is successful requests per joule
summed over the boards listed in energy.csv.

A configuration qualifies only if its mean success rate >= --min-success.
View changes, state fetches and reboots are part of a battery run (a replica
whose battery empties first forces a view change): they are counted per run
and shown, and a run is dropped for them only with --exclude-faulty.  A
replica that joined late (plugged in after the others) is a note.  Replica
counters are totals since boot.  "resent/run" counts every message sent again
(client requests and replica re-sends); "drops/run" counts sends given up and
messages or fragments thrown away.
"Best" means best among the points you ran, not a proven optimum.

  analyze.py runs/ [--min-success 0.95]
"""
import argparse
import csv
import glob
import os
import re
import statistics as st

ANSI = re.compile(r"\x1b\[[0-9;]*m")
KV = re.compile(r"(\w+)=(-?\d+)")
BOOT_MS = re.compile(r"\b[IWED] \((\d+)\) ")
BANNER_KV = re.compile(r"(\w+)=(\S+)")
RATE_MBPS = {0: 1, 11: 6, 16: 6.5, 9: 24, 12: 54, 23: 65}


def parse_log(path):
    """Every BANNER, CLIENT_STATS and TBFT_STATS line, in order."""
    out = {"banners": [], "client": [], "stats": [], "events": []}
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        if "CLIENT_STATS" in line:
            d = dict((k, int(v)) for k, v in KV.findall(line.split("CLIENT_STATS", 1)[1]))
            if "ok" in d and "fail" in d:
                m = BOOT_MS.search(line)
                d["_t"] = int(m.group(1)) if m else None   # ms since the client booted
                out["client"].append(d)
                out["events"].append(("client", len(out["client"]) - 1))
        elif "TBFT_STATS" in line:
            out["stats"].append(dict((k, int(v)) for k, v in KV.findall(line.split("TBFT_STATS", 1)[1])))
            out["events"].append(("stats", len(out["stats"]) - 1))
        elif "BANNER" in line:
            out["banners"].append(dict(BANNER_KV.findall(line.split("BANNER", 1)[1])))
            out["events"].append(("banner", len(out["banners"]) - 1))
    out["banner"] = out["banners"][0] if out["banners"] else None
    return out


def energy(path):
    tot, nodes, secs = 0.0, set(), 0.0
    for row in csv.DictReader(open(path)):
        secs = max(secs, float(row.get("seconds") or 0))
        if row.get("joules"):
            j = float(row["joules"])
        else:
            j = float(row["avg_ma"]) / 1000 * float(row["volts"]) * float(row["seconds"])
        nodes.add(row["node"])
        tot += j
    return tot, frozenset(nodes), secs


def load_run(d):
    logs = {os.path.splitext(os.path.basename(p))[0]: parse_log(p)
            for p in glob.glob(os.path.join(d, "*.log"))}
    r = {"dir": d, "ok": 0, "total": 0, "faulty": [], "notes": [], "avg_ms": None,
         "max_ms": None, "resent": 0, "drops": 0, "banner": None}
    client = next((v for v in logs.values() if v["client"]), None)
    if client:
        r["banner"] = client["banner"]
    r["banner"] = r["banner"] or next((v["banner"] for v in logs.values() if v["banner"]), None)
    rows = client["client"] if client else []
    # The counters restart at 0 when the client reboots (a brown-out at the
    # end of a battery): split the rows into boots and add up their totals.
    boots = []
    for x in rows:
        if not boots or x["ok"] < boots[-1][-1]["ok"] or x["fail"] < boots[-1][-1]["fail"]:
            boots.append([])
        boots[-1].append(x)
    if len(boots) > 1 or (client and len(client["banners"]) > 1):
        r["faulty"].append(f"client rebooted ({max(len(boots), len(client['banners'])) - 1} times, "
                           "counters summed)")
    # The client may stay powered between runs while only the replicas run on
    # batteries.  If its log starts with CLIENT_STATS and no BANNER before it,
    # the client was already running: that first row (earlier runs' counts)
    # is the baseline, and the run is counted from the start of the log.
    kinds = [k for k, _ in client["events"] if k in ("client", "banner")] if client else []
    zero = {"ok": 0, "fail": 0, "missed": 0, "_t": 0}
    base = rows[0] if kinds and kinds[0] == "client" else zero
    if base is not zero:
        r["notes"].append("client was already running when the log started: counted from the "
                          f"log's first CLIENT_STATS (ok={base['ok']} fail={base['fail']} before)")
    starts = [base] + [zero] * (len(boots) - 1)       # each boot counts from its start row
    firsts = [1 if base is not zero else 0] + [0] * (len(boots) - 1)
    r["ok"] = sum(bt[-1]["ok"] - st0["ok"] for bt, st0 in zip(boots, starts))
    r["fail"] = sum(bt[-1]["fail"] - st0["fail"] for bt, st0 in zip(boots, starts))
    # A "missed" slot is one the client could not send because the previous
    # request was still running: it measures latency, not loss, so it is kept
    # apart from the failure count.
    r["missed"] = sum(bt[-1].get("missed", 0) - st0.get("missed", 0)
                      for bt, st0 in zip(boots, starts))
    r["total"] = r["ok"] + r["fail"] + r["missed"]
    r["run_s"] = sum(((bt[-1].get("_t") or 0) - (st0.get("_t") or 0))
                     for bt, st0 in zip(boots, starts)) / 1000 or None
    # Latency of every successful request (last_ms of its own row, the first
    # success after boot included), not the firmware's running max.
    lat, steady = [], []                   # steady: without each boot's first success
    for bt, st0, i0 in zip(boots, starts, firsts):
        prev_ok = st0["ok"]
        for x in bt[i0:]:
            if x["ok"] > prev_ok and x.get("last_ms") is not None:
                lat.append(x["last_ms"])
                if prev_ok > st0["ok"]:
                    steady.append(x["last_ms"])
            prev_ok = x["ok"]
    if lat:
        r["avg_ms"], r["max_ms"] = round(sum(lat) / len(lat)), max(lat)
    # For the re-send check only: the first success after a boot waits for
    # the cluster to come up, so it says nothing about the re-send interval.
    r["steady_max_ms"] = max(steady) if steady else None
    # Warm-up: when the run's first success came (seconds from the client's
    # boot, or from the start of the log), and how many requests failed before.
    first = next((x for x in boots[0] if x["ok"] > base["ok"]), None) if boots else None
    if first and first.get("_t") is not None:
        r["first_ok_s"] = (first["_t"] - (base.get("_t") or 0)) / 1000
    r["warm_fail"] = ((first["fail"] if first else boots[0][-1]["fail"]) - base["fail"]
                      if boots else None)
    sb = client["stats"][-1] if client and client["stats"] else None
    r["vc"] = max(sb.get("view", 0), 0) if sb else 0   # views moved through, as the client saw
    if r["vc"]:
        r["faulty"].append(f"view change (client saw view {sb.get('view')})")
    if sb:
        def sent_again(x):
            return x.get("req_resent", 0)

        def dropped(x):
            return (x.get("tx_fail", 0) + x.get("tx_qfull", 0) + x.get("rx_drop", 0)
                    + x.get("rx_frag_drop", 0))
        # Already running at the start of the log: its first TBFT_STATS is the
        # baseline too (only when the client did not reboot in between).
        s0 = client["stats"][0] if base is not zero and len(boots) == 1 else {}
        r["resent"] += sent_again(sb) - sent_again(s0)
        r["drops"] += dropped(sb) - dropped(s0)

    replicas = {n: v for n, v in logs.items() if v is not client and v["stats"]}
    r["fetch"] = 0
    if not replicas:
        r["notes"].append("no replica TBFT_STATS: a state fetch cannot be detected")
    for name, v in sorted(replicas.items()):
        s = v["stats"][-1]                         # totals since this replica's boot
        if len(v["banners"]) > 1:
            r["faulty"].append(f"{name} rebooted")
        if s.get("first_exec", 0) > 1:
            r["notes"].append(f"{name} joined late (first executed seqno {s['first_exec']})")
        r["fetch"] += s.get("fetch", 0)
        if s.get("fetch", 0):
            r["faulty"].append(f"{name}: state fetch x{s['fetch']}")
        r["resent"] += s.get("resent", s.get("commit_resent", 0))
        r["drops"] += (s.get("tx_fail", 0) + s.get("tx_qfull", 0) + s.get("rx_drop", 0)
                       + s.get("rx_frag_drop", 0))
        if rows and s.get("last_exec", 0) < r["ok"]:
            r["notes"].append(f"{name}: last TBFT_STATS printed before the run ended "
                              f"(last_exec {s.get('last_exec')} < {r['ok']} requests)")
    f = os.path.join(d, "energy.csv")
    if os.path.exists(f):
        r["energy"], r["nodes"], r["secs"] = energy(f)
        if r["secs"] and r.get("run_s") and abs(r["secs"] - r["run_s"]) > 0.05 * r["run_s"]:
            r["notes"].append(f"energy.csv covers {r['secs']:.0f} s but the client ran "
                              f"{r['run_s']:.0f} s from boot: measure from power-on to the end")
    return r


def point_of(name):
    return re.sub(r"-(r\d+|base)$", "", name)


def banner_int(b, key):
    try:
        return int(b.get(key))
    except (TypeError, ValueError):
        return None


def mean_sd(v):
    return (st.mean(v), st.stdev(v) if len(v) > 1 else 0.0) if v else (None, 0.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs")
    ap.add_argument("--min-success", type=float, default=0.95)
    ap.add_argument("--exclude-faulty", action="store_true",
                    help="drop runs with a view change, state fetch or reboot")
    a = ap.parse_args()

    runs, base = {}, {}
    for d in sorted(glob.glob(os.path.join(a.runs, "*"))):
        if os.path.isdir(d):
            n = os.path.basename(d)
            (base if n.endswith("-base") else runs).setdefault(point_of(n), []).append(load_run(d))

    node_sets = {r["nodes"] for rs in runs.values() for r in rs if "nodes" in r}
    if len(node_sets) > 1:
        print("WARNING: energy.csv lists different boards in different runs "
              f"({sorted(sorted(s) for s in node_sets)}); req/J is not comparable across them.\n")
    for pid, bs in base.items():
        for b in bs:
            run_nodes = {r["nodes"] for r in runs.get(pid, []) if "nodes" in r}
            if "nodes" in b and run_nodes and b["nodes"] not in run_nodes:
                print(f"WARNING: baseline {b['dir']} measured other boards than the runs of {pid}.")

    # settings of each configuration, from the boot BANNER of its runs
    meta = {pid: next((r["banner"] for r in rs if r["banner"]), {}) for pid, rs in runs.items()}
    for pid, b in meta.items():
        if not b:
            print(f"WARNING: no BANNER line in the logs of {pid}: transport unknown.")

    for pid, rs in runs.items():
        for r in rs:
            for why in r["faulty"]:
                print(f"{os.path.basename(r['dir'])}: "
                      + ("excluded" if a.exclude_faulty else "fault (counted)") + f" - {why}")
            for note in r["notes"]:
                print(f"{os.path.basename(r['dir'])}: note - {note}")
        retx = banner_int(meta[pid], "retx_ms")
        slow = [r["steady_max_ms"] for r in rs if r.get("steady_max_ms")]
        if retx and slow and max(slow) >= 0.8 * retx:
            print(f"WARNING: {pid}: slowest request {max(slow)} ms is close to the client re-send "
                  f"interval ({retx} ms): re-sends are not only losses; raise "
                  f"TBFT_CLIENT_RETRANSMIT_MS for every configuration")

    rows = {}
    for pid, rs in runs.items():
        counted = [r for r in rs if r["total"] > 0]
        used = [r for r in counted if not (a.exclude_faulty and r["faulty"])]
        m = {"n": len(used), "of": len(rs), "dropped": len(counted) - len(used),
             "succ": None, "slot": None, "eff": None, "eff_sd": 0.0, "jpr": None,
             "inc": None, "lat": None, "max": None, "resent": None, "drops": None,
             "run_s": None, "first_ok": None, "warm_fail": None, "vc": None, "fetch": None}
        if used:
            # Two numbers, because they answer different questions:
            #   succ = requests that were sent and succeeded (loss and faults)
            #   slot = scheduled slots that succeeded (latency included)
            sent = [r for r in used if r["ok"] + r["fail"] > 0]
            m["succ"] = st.mean(r["ok"] / (r["ok"] + r["fail"]) for r in sent) if sent else None
            m["slot"] = st.mean(r["ok"] / r["total"] for r in used)
            m["resent"] = st.mean(r["resent"] for r in used)
            m["drops"] = st.mean(r["drops"] for r in used)
            for k, rk in (("run_s", "run_s"), ("first_ok", "first_ok_s"), ("warm_fail", "warm_fail"),
                          ("vc", "vc"), ("fetch", "fetch")):
                v = [r[rk] for r in used if r.get(rk) is not None]
                m[k] = st.mean(v) if v else None
            lat = [r["avg_ms"] for r in used if r["avg_ms"]]
            m["lat"] = st.mean(lat) if lat else None
            mx = [r["max_ms"] for r in used if r["max_ms"]]
            m["max"] = max(mx) if mx else None
            with_e = [r for r in used if "energy" in r and r["ok"] > 0]
            m["eff"], m["eff_sd"] = mean_sd([r["ok"] / r["energy"] for r in with_e])
            m["jpr"], _ = mean_sd([r["energy"] / r["ok"] for r in with_e])
            b = next((x for x in base.get(pid, []) if x.get("secs")), None)
            if b:
                inc = [(r["energy"] - b["energy"] * r["secs"] / b["secs"]) / r["ok"]
                       for r in with_e if r.get("secs")]
                m["inc"], _ = mean_sd(inc)
        rows[pid] = m

    def fmt(x, spec):
        return format(x, spec) if x is not None else "-"

    print("\n| point | runs | run s | first OK s | warm-up fails | success (sent) | slots "
          "| avg latency ms | max latency ms | req/J (±sd) | J/req | incr J/req | view changes "
          "| fetches | resent/run | drops/run | excluded |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for pid, m in sorted(rows.items(), key=lambda kv: -(kv[1]["eff"] or -1)):
        eff = f"{m['eff']:.4f} ±{m['eff_sd']:.4f}" if m["eff"] is not None else "-"
        print(f"| {pid} | {m['n']}/{m['of']} | {fmt(m['run_s'], '.0f')} | {fmt(m['first_ok'], '.1f')} "
              f"| {fmt(m['warm_fail'], '.1f')} | {fmt(m['succ'], '.1%')} | {fmt(m['slot'], '.1%')} "
              f"| {fmt(m['lat'], '.0f')} | {fmt(m['max'], '.0f')} | {eff} | {fmt(m['jpr'], '.2f')} "
              f"| {fmt(m['inc'], '.2f')} | {fmt(m['vc'], '.1f')} | {fmt(m['fetch'], '.1f')} "
              f"| {fmt(m['resent'], '.1f')} | {fmt(m['drops'], '.1f')} | {m['dropped']} |")

    # ESP-NOW rate screen: with an always-on radio the rate barely moves req/J,
    # so choose by success rate, then by speed (shorter airtime).
    screen = [(pid, m) for pid, m in rows.items()
              if meta[pid].get("transport") == "espnow" and banner_int(meta[pid], "window_ms") == 0
              and banner_int(meta[pid], "bcast") == 0 and banner_int(meta[pid], "retries") == 3
              and m["succ"] is not None]
    if screen:
        ok = [(pid, m) for pid, m in screen if m["succ"] >= a.min_success]
        ok.sort(key=lambda kv: -RATE_MBPS.get(banner_int(meta[kv[0]], "rate"), 0))
        if ok:
            picks = ", ".join(f"{RATE_MBPS.get(banner_int(meta[pid], 'rate'), '?')} Mbps"
                              for pid, _ in ok[:2])
            print(f"\nESP-NOW rate screen: fastest rates with success >= {a.min_success:.0%}: "
                  + ", ".join(f"{pid} ({m['succ']:.1%})" for pid, m in ok[:2])
                  + f"\n  -> next: ESP-NOW receive power saving at PHY rate {picks}")
        else:
            print(f"\nESP-NOW rate screen: no rate reached {a.min_success:.0%} success")

    print(f"\nBest per transport (success >= {a.min_success:.0%}, by req/J, among the points run):")
    best = {}
    for pid, m in rows.items():
        if m["eff"] is None or m["succ"] < a.min_success:
            continue
        t = meta[pid].get("transport", "unknown")
        if t not in best or m["eff"] > best[t][1]["eff"]:
            best[t] = (pid, m)
    for t, (pid, m) in best.items():
        print(f"- {t}: {pid}  {m['eff']:.4f} req/J (±{m['eff_sd']:.4f}, n={m['n']}), "
              f"success {m['succ']:.1%}")
    if len(best) == 2:
        (_, m1), (_, m2) = best.values()
        hi, lo = (m1, m2) if m1["eff"] >= m2["eff"] else (m2, m1)
        overlap = (lo["eff"] + lo["eff_sd"]) >= (hi["eff"] - hi["eff_sd"])
        few = min(hi["n"], lo["n"]) < 3
        print(f"\nRatio best/other = {hi['eff'] / lo['eff']:.2f}x"
              + ("  - within one stdev: NOT a significant difference" if overlap else "")
              + ("  - fewer than 3 runs on one side: treat as a screen, not a result" if few else ""))
    elif not best:
        print("- no configuration with energy data met the success floor")


if __name__ == "__main__":
    main()
