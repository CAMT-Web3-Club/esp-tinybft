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

Requests are counted from the first successful request to the last
CLIENT_STATS line, so start the meter at the first successful request and stop
it at the end.  Average and maximum latency are those of the successful
requests from the second one on: the first request after boot waits for the
whole cluster to come up and is reported in a note instead.  Measure the SAME boards in every run: efficacy is successful
requests per joule summed over the boards listed in energy.csv.

A configuration qualifies only if its mean success rate >= --min-success.
Runs with a view change, a state fetch, a board that rebooted or a replica
that joined late are excluded unless --include-faulty.  Replica counters are
taken from the replica's first executed request on (run_* in TBFT_STATS), so
the boot phase does not count.  "resent/run" counts every message sent again
(client requests and replica re-sends); "drops/run" counts sends given up and
messages or fragments thrown away.  Both are normally 0; a request slower than
the client's re-send interval also adds re-sends (a warning says so).
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
    """Every BANNER, CLIENT_STATS and TBFT_STATS line, in order."""
    out = {"banners": [], "client": [], "stats": [], "events": []}
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        if "CLIENT_STATS" in line:
            d = dict((k, int(v)) for k, v in KV.findall(line.split("CLIENT_STATS", 1)[1]))
            if "ok" in d and "fail" in d:
                out["client"].append(d)
                out["events"].append(("client", len(out["client"]) - 1))
        elif "TBFT_STATS" in line:
            out["stats"].append(dict((k, int(v)) for k, v in KV.findall(line.split("TBFT_STATS", 1)[1])))
            out["events"].append(("stats", len(out["stats"]) - 1))
        elif "BANNER" in line:
            out["banners"].append(dict(BANNER_KV.findall(line.split("BANNER", 1)[1])))
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


def stats_after(log, client_idx):
    """The client's TBFT_STATS line printed right after CLIENT_STATS row i."""
    seen = False
    for kind, i in log["events"]:
        if kind == "client" and i == client_idx:
            seen = True
        elif kind == "stats" and seen:
            return log["stats"][i]
    return None


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
    if client and (len(client["banners"]) > 1 or
                   any(b["ok"] < a["ok"] for a, b in zip(rows, rows[1:]))):
        r["faulty"].append("client rebooted")
    first = next((i for i, x in enumerate(rows) if x["ok"] > 0), None)
    if first is not None:
        a, b = rows[first], rows[-1]
        r["ok"] = b["ok"] - a["ok"] + 1          # the first success opens the window
        r["total"] = r["ok"] + (b["fail"] - a["fail"]) + (b.get("missed", 0) - a.get("missed", 0))
        # Latency of each successful request (last_ms of its own row), not the
        # firmware's running max: that one keeps the first request after boot,
        # which waits for the whole cluster to come up (5-9 s on the boards)
        # and would make every run look slower than the client re-send
        # interval.  The boot request is reported separately.
        r["boot_ms"] = rows[first].get("last_ms")
        lat, prev = [], rows[first]
        for x in rows[first + 1:]:
            if x["ok"] > prev["ok"] and x.get("last_ms") is not None:
                lat.append(x["last_ms"])
            prev = x
        if lat:
            r["avg_ms"], r["max_ms"] = round(sum(lat) / len(lat)), max(lat)
        else:
            r["avg_ms"], r["max_ms"] = r["boot_ms"], r["boot_ms"]
        if lat and r["boot_ms"] and r["boot_ms"] > max(lat):
            r["notes"].append(f"the first request after boot took {r['boot_ms']} ms "
                              f"(excluded from avg/max latency)")
        sa, sb = stats_after(client, first), (client["stats"][-1] if client["stats"] else None)
        if sa and sb:
            if sb.get("view", -1) > sa.get("view", -1):
                r["faulty"].append(f"view change (client saw view {sa.get('view')} -> {sb.get('view')})")
            r["resent"] += sb.get("req_resent", 0) - sa.get("req_resent", 0)
            r["drops"] += ((sb.get("tx_fail", 0) + sb.get("tx_qfull", 0) + sb.get("rx_drop", 0)
                            + sb.get("rx_frag_drop", 0))
                           - (sa.get("tx_fail", 0) + sa.get("tx_qfull", 0) + sa.get("rx_drop", 0)
                              + sa.get("rx_frag_drop", 0)))
        elif sb:                                   # firmware without per-request client stats
            r["resent"] += sb.get("req_resent", 0)
    elif rows:                                   # client ran but nothing ever succeeded
        r["total"] = rows[-1]["fail"] + rows[-1].get("missed", 0)

    replicas = {n: v for n, v in logs.items() if v is not client and v["stats"]}
    if not replicas:
        r["notes"].append("no replica TBFT_STATS: a state fetch cannot be detected")
    for name, v in sorted(replicas.items()):
        s = v["stats"][-1]
        if len(v["banners"]) > 1:
            r["faulty"].append(f"{name} rebooted")
        if "run_vc" in s:                          # counted from the first executed request
            if s.get("first_exec", 0) > 1:
                r["faulty"].append(f"{name} joined late (first executed seqno {s['first_exec']})")
            ev = (s.get("run_vc", 0), s.get("run_nv", 0), s.get("run_fetch", 0))
            r["resent"] += s.get("run_resent", 0)
            r["drops"] += s.get("run_tx_fail", 0) + s.get("run_drop", 0)
        else:                                      # older firmware: totals since boot
            ev = (s.get("vc_sent", 0), s.get("new_view", 0), s.get("fetch", 0))
            r["resent"] += s.get("commit_resent", 0)
        if any(ev):
            r["faulty"].append(f"{name}: view-change/new-view/fetch {ev[0]}/{ev[1]}/{ev[2]}")
        if rows and s.get("last_exec", 0) < rows[-1]["ok"]:
            r["notes"].append(f"{name}: last TBFT_STATS printed before the run ended "
                              f"(last_exec {s.get('last_exec')} < {rows[-1]['ok']} requests)")
    f = os.path.join(d, "energy.csv")
    if os.path.exists(f):
        r["energy"], r["nodes"], r["secs"] = energy(f)
        period = banner_int(r["banner"] or {}, "req_interval_ms")
        if r["secs"] and period and r["total"]:
            window = r["total"] * period / 1000
            if abs(r["secs"] - window) > max(2 * period / 1000, 0.05 * window):
                r["notes"].append(f"energy.csv covers {r['secs']:.0f} s but the requests span "
                                  f"{window:.0f} s: start the meter at the first success")
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
                print(f"{os.path.basename(r['dir'])}: excluded - {why}"
                      + (" (kept: --include-faulty)" if a.include_faulty else ""))
            for note in r["notes"]:
                print(f"{os.path.basename(r['dir'])}: note - {note}")
        retx = banner_int(meta[pid], "retx_ms")
        slow = [r["max_ms"] for r in rs if r["max_ms"]]
        if retx and slow and max(slow) >= 0.8 * retx:
            print(f"WARNING: {pid}: slowest request {max(slow)} ms is close to the client re-send "
                  f"interval ({retx} ms): re-sends are not only losses; raise "
                  f"TBFT_CLIENT_RETRANSMIT_MS for every configuration")

    rows = {}
    for pid, rs in runs.items():
        counted = [r for r in rs if r["total"] > 0]
        used = [r for r in counted if a.include_faulty or not r["faulty"]]
        m = {"n": len(used), "of": len(rs), "dropped": len(counted) - len(used),
             "succ": None, "eff": None, "eff_sd": 0.0, "jpr": None, "inc": None, "lat": None,
             "max": None, "resent": None, "drops": None}
        if used:
            m["succ"] = st.mean(r["ok"] / r["total"] for r in used)
            m["resent"] = st.mean(r["resent"] for r in used)
            m["drops"] = st.mean(r["drops"] for r in used)
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

    print("\n| point | runs | success | avg latency ms | max latency ms | req/J (±sd) | J/req "
          "| incr J/req | resent/run | drops/run | excl. faulty |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for pid, m in sorted(rows.items(), key=lambda kv: -(kv[1]["eff"] or -1)):
        eff = f"{m['eff']:.4f} ±{m['eff_sd']:.4f}" if m["eff"] is not None else "-"
        print(f"| {pid} | {m['n']}/{m['of']} | {fmt(m['succ'], '.1%')} | {fmt(m['lat'], '.0f')} "
              f"| {fmt(m['max'], '.0f')} | {eff} | {fmt(m['jpr'], '.2f')} | {fmt(m['inc'], '.2f')} "
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
