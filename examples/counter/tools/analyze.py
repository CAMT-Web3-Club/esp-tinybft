#!/usr/bin/env python3
"""Summarise measured runs and pick the best configuration per transport.

Optional helper: the firmware and menuconfig do not need it.

Layout (one directory per run; you choose the configuration name, repeats are
named <name>-r1, -r2, ...):
  runs/<name>-r1/client7.log      serial logs, one file per board; the file
  runs/<name>-r1/node0.log ...    stem must match the node label in energy.csv
  runs/<name>-r1/energy.csv       node,joules[,seconds]  OR  node,avg_ma,volts,seconds
                                  (seconds is needed for the incremental column)
  runs/<name>-base/               optional baseline: same image, client OFF
                                  (energy.csv only; replica logs optional)

The settings of each configuration (transport, rate, power saving, ...) are
read from the BANNER line the boards print at boot, so keep the boot output
in the client log.

Only the client log is required.  Without energy.csv a run still contributes
success rate and latency (enough for the stage-1 ESP-NOW rate screen).

Requests are counted from the first successful request to the last
CLIENT_STATS line, so start the meter at the first successful request and stop
it at the end.  Measure the SAME boards in every run: efficacy is successful
requests per joule summed over the boards listed in energy.csv.

A configuration qualifies only if its mean success rate >= --min-success.
Runs with view-changes or state fetches are excluded unless --include-faulty.
The "resent/run" column counts re-sent requests (client) and commits
(replicas): messages that were lost and recovered, kept in the result but
worth reporting.
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
BANNER_KV = re.compile(r"(\w+)=(\S+)")
RATE_MBPS = {0: 1, 11: 6, 16: 6.5, 9: 24, 12: 54, 23: 65}


def parse_log(path):
    out = {"client": [], "stats": None, "banner": None}
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        if "CLIENT_STATS" in line:
            d = dict((k, int(v)) for k, v in KV.findall(line))
            if "ok" in d and "fail" in d:
                out["client"].append(d)
        elif "TBFT_STATS" in line:
            out["stats"] = dict((k, int(v)) for k, v in KV.findall(line))
        elif "BANNER" in line:
            out["banner"] = dict(BANNER_KV.findall(line.split("BANNER", 1)[1]))
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
    r = {"dir": d, "ok": 0, "total": 0, "faulty": False, "avg_ms": None, "resent": 0,
         "banner": next((v["banner"] for v in logs.values() if v["banner"]), None)}
    rows = next((v["client"] for v in logs.values() if v["client"]), [])
    first = next((i for i, x in enumerate(rows) if x["ok"] > 0), None)
    if first is not None:
        a, b = rows[first], rows[-1]
        r["ok"] = b["ok"] - a["ok"] + 1          # the first success opens the window
        r["total"] = r["ok"] + (b["fail"] - a["fail"]) + (b.get("missed", 0) - a.get("missed", 0))
        r["avg_ms"] = b.get("avg_ms")
    elif rows:                                   # client ran but nothing ever succeeded
        r["total"] = rows[-1]["fail"] + rows[-1].get("missed", 0)
    for v in logs.values():
        s = v["stats"] or {}
        if s.get("vc_sent", 0) or s.get("new_view", 0) or s.get("fetch", 0):
            r["faulty"] = True
        r["resent"] += s.get("req_resent", 0) + s.get("commit_resent", 0)
    f = os.path.join(d, "energy.csv")
    if os.path.exists(f):
        r["energy"], r["nodes"], r["secs"] = energy(f)
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
    ap.add_argument("--include-faulty", action="store_true")
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

    # settings of each configuration, from the boot BANNER of its runs
    meta = {pid: next((r["banner"] for r in rs if r["banner"]), {}) for pid, rs in runs.items()}
    for pid, b in meta.items():
        if not b:
            print(f"WARNING: no BANNER line in the logs of {pid}: transport unknown.")

    rows = {}
    for pid, rs in runs.items():
        counted = [r for r in rs if r["total"] > 0]
        used = [r for r in counted if a.include_faulty or not r["faulty"]]
        m = {"n": len(used), "of": len(rs), "dropped": len(counted) - len(used),
             "succ": None, "eff": None, "eff_sd": 0.0, "jpr": None, "inc": None, "lat": None,
             "resent": None}
        if used:
            m["succ"] = st.mean(r["ok"] / r["total"] for r in used)
            m["resent"] = st.mean(r["resent"] for r in used)
            lat = [r["avg_ms"] for r in used if r["avg_ms"]]
            m["lat"] = st.mean(lat) if lat else None
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

    print("| point | runs | success | avg latency ms | req/J (±sd) | J/req | incr J/req | resent/run | excl. faulty |")
    print("|---|---|---|---|---|---|---|---|---|")
    for pid, m in sorted(rows.items(), key=lambda kv: -(kv[1]["eff"] or -1)):
        eff = f"{m['eff']:.4f} ±{m['eff_sd']:.4f}" if m["eff"] is not None else "-"
        print(f"| {pid} | {m['n']}/{m['of']} | {fmt(m['succ'], '.1%')} | {fmt(m['lat'], '.0f')} "
              f"| {eff} | {fmt(m['jpr'], '.2f')} | {fmt(m['inc'], '.2f')} | {fmt(m['resent'], '.1f')} "
              f"| {m['dropped']} |")

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
