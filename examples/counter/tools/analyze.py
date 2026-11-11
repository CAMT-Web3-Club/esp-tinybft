#!/usr/bin/env python3
"""Summarise sweep runs and pick the best configuration per transport.

Layout (one directory per run, repeats named <point-id>-r1, -r2, ...):
  runs/<point-id>-r1/client7.log      serial logs, one file per board; the stem
  runs/<point-id>-r1/node0.log ...    must match the node label in energy.csv
  runs/<point-id>-r1/energy.csv       node,joules[,seconds]  OR  node,avg_ma,volts,seconds
                                      (seconds is needed for the incremental column)
  runs/<point-id>-base/               optional baseline: same image, client OFF
                                      (replica logs + energy only)

Energy must cover the same window as the request counts: this script counts
requests from the first successful request to the last CLIENT_STATS line, so
start the meter at the first successful request and stop it at the end.

Efficacy = successful requests per joule over all boards in the run.
A configuration only qualifies if its mean success rate >= --min-success.
Runs with view-changes / state fetches are excluded unless --include-faulty.

  analyze.py runs/ [--manifest sweeps/manifest.json] [--min-success 0.95]
"""
import argparse, csv, glob, json, os, re, statistics as st, sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
KV = re.compile(r"(\w+)=(-?\d+)")


def parse_log(path):
    out = {"client": [], "stats": None, "banner": None}
    for line in open(path, errors="replace"):
        line = ANSI.sub("", line)
        if "CLIENT_STATS" in line:
            d = dict((k, int(v)) for k, v in KV.findall(line))
            if "ok" in d:
                out["client"].append(d)
        elif "TBFT_STATS" in line:
            out["stats"] = dict((k, int(v)) for k, v in KV.findall(line))
        elif "BANNER" in line:
            out["banner"] = line.strip()
    return out


def energy(path):
    tot, per = 0.0, {}
    secs = 0.0
    for row in csv.DictReader(open(path)):
        secs = max(secs, float(row.get("seconds") or 0))
        if row.get("joules"):
            j = float(row["joules"])
        else:
            j = float(row["avg_ma"]) / 1000 * float(row["volts"]) * float(row["seconds"])
        per[row["node"]] = j
        tot += j
    return tot, per, secs


def load_run(d):
    logs = {os.path.splitext(os.path.basename(p))[0]: parse_log(p)
            for p in glob.glob(os.path.join(d, "*.log"))}
    r = {"dir": d, "logs": logs, "ok": 0, "fail": 0, "missed": 0, "faulty": False}
    cl = [v for v in logs.values() if v["client"]]
    if cl:
        rows = cl[0]["client"]
        first = next((i for i, x in enumerate(rows) if x["ok"] > 0), None)
        if first is not None:
            a, b = rows[first], rows[-1]
            r["ok"] = b["ok"] - a["ok"] + 1       # the first success opens the window
            r["fail"] = b["fail"] - a["fail"]
            r["missed"] = b["missed"] - a["missed"]
            r["avg_ms"] = b.get("avg_ms")
    for v in logs.values():
        s = v["stats"] or {}
        if s.get("vc_sent", 0) or s.get("new_view", 0) or s.get("fetch", 0):
            r["faulty"] = True
    f = os.path.join(d, "energy.csv")
    if os.path.exists(f):
        r["energy"], r["per_node"], r["secs"] = energy(f)
    return r


def point_of(name):
    return re.sub(r"-(r\d+|base)$", "", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--manifest", default="sweeps/manifest.json")
    ap.add_argument("--min-success", type=float, default=0.95)
    ap.add_argument("--include-faulty", action="store_true")
    a = ap.parse_args()
    man = json.load(open(a.manifest)) if os.path.exists(a.manifest) else {}

    runs, base = {}, {}
    for d in sorted(glob.glob(os.path.join(a.runs, "*"))):
        if not os.path.isdir(d):
            continue
        n = os.path.basename(d)
        r = load_run(d)
        if n.endswith("-base"):
            base.setdefault(point_of(n), []).append(r)
        else:
            runs.setdefault(point_of(n), []).append(r)

    rows = []
    for pid, rs in runs.items():
        good = [r for r in rs if "energy" in r and (r["ok"] + r["fail"] + r["missed"]) > 0]
        used = [r for r in good if a.include_faulty or not r["faulty"]]
        dropped = len(good) - len(used)
        if not used:
            rows.append((pid, None, dropped, len(rs)))
            continue
        succ = [r["ok"] / (r["ok"] + r["fail"] + r["missed"]) for r in used]
        eff = [r["ok"] / r["energy"] for r in used if r["ok"] > 0]
        jpr = [r["energy"] / r["ok"] for r in used if r["ok"] > 0]
        inc = None
        b = base.get(pid)
        if b and "energy" in b[0] and b[0].get("secs"):
            xs = []
            for r in used:
                if r["ok"] and r.get("secs"):
                    e0 = b[0]["energy"] * r["secs"] / b[0]["secs"]
                    xs.append((r["energy"] - e0) / r["ok"])
            inc = st.mean(xs) if xs else None
        sd = lambda v: st.stdev(v) if len(v) > 1 else 0.0
        rows.append((pid, dict(n=len(used), succ=st.mean(succ), eff=st.mean(eff) if eff else 0,
                               eff_sd=sd(eff), jpr=st.mean(jpr) if jpr else float("inf"),
                               jpr_sd=sd(jpr), inc=inc), dropped, len(rs)))

    print("| point | runs | success | req/J (±sd) | J/req | incr J/req | excl. faulty |")
    print("|---|---|---|---|---|---|---|")
    for pid, m, dropped, n in sorted(rows, key=lambda x: -(x[1]["eff"] if x[1] else -1)):
        if m is None:
            print(f"| {pid} | 0/{n} usable | - | - | - | - | {dropped} |")
            continue
        inc = f"{m['inc']:.2f}" if m["inc"] is not None else "-"
        print(f"| {pid} | {m['n']} | {m['succ']:.1%} | {m['eff']:.4f} ±{m['eff_sd']:.4f} "
              f"| {m['jpr']:.2f} | {inc} | {dropped} |")

    print(f"\nBest per transport (success >= {a.min_success:.0%}, by req/J):")
    best = {}
    for pid, m, _, _ in rows:
        if not m or m["succ"] < a.min_success:
            continue
        t = (man.get(pid) or {}).get("transport") or ("espnow" if pid.startswith("en") else "udp")
        if t not in best or m["eff"] > best[t][1]["eff"]:
            best[t] = (pid, m)
    for t, (pid, m) in best.items():
        print(f"- {t}: {pid}  {m['eff']:.4f} req/J (±{m['eff_sd']:.4f}, n={m['n']}), "
              f"success {m['succ']:.1%}")
    if len(best) == 2:
        (_, a1), (_, a2) = best.values()
        hi, lo = (a1, a2) if a1["eff"] >= a2["eff"] else (a2, a1)
        overlap = (lo["eff"] + lo["eff_sd"]) >= (hi["eff"] - hi["eff_sd"])
        print(f"\nRatio best/other = {hi['eff'] / lo['eff']:.2f}x"
              + ("  - within one stdev: NOT a significant difference" if overlap else ""))
    elif not best:
        print("- no configuration met the success floor")


if __name__ == "__main__":
    main()
