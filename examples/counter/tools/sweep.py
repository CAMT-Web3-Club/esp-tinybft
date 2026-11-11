#!/usr/bin/env python3
"""Plan and build the parameter sweep for the best-vs-best comparison.

  sweep.py plan  [--stage 1|2|3] [--rate N] [--listen N]   # write overlays + manifest
  sweep.py list                                              # show planned points
  sweep.py build [--stage ..] [--only ID ...] [--roles replica client]

Each point is a self-contained overlay (sweeps/<id>.overlay) on top of a base
variant; images go to build_sweep/<id>_<role>/.  Run inside an activated IDF
shell (idf.py on PATH).  Flashing and measuring stay manual: see README.

Stages
  1  ESP-NOW PHY-rate sweep (retries 3, PM off) + every UDP PS mode x PM on/off
  2  ESP-NOW with the chosen --rate: retries {3,8} x PM {off,on} x
     {always-on, duty-cycled 50/100, 20/100}
  3  UDP PS_MAX listen interval {1,3,10} with PM on
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SWEEPS = os.path.join(ROOT, "sweeps")
MANIFEST = os.path.join(SWEEPS, "manifest.json")

PM = ["CONFIG_PM_ENABLE=y", "CONFIG_FREERTOS_USE_TICKLESS_IDLE=y"]
# wifi_phy_rate_t values accepted by TBFT_ESPNOW_PHY_RATE
RATES = {0: "idf-default(1M)", 11: "6M", 9: "24M", 12: "54M", 16: "MCS0", 23: "MCS7"}


def point(pid, transport, base, **kw):
    lines = []
    p = dict(id=pid, transport=transport, base=base, pm=kw.get("pm", False),
             rate=None, retries=None, window=None, interval=None, ps=None, listen=None)
    if transport == "espnow":
        p["rate"] = kw.get("rate", 0)
        p["retries"] = kw.get("retries", 3)
        cfg = {"CONFIG_TBFT_ESPNOW_PHY_RATE": p["rate"]}
        if kw.get("window"):
            p["window"], p["interval"] = kw["window"], kw["interval"]
            # duty-cycled receivers: attempts (x 20 ms) must span one wake interval
            p["retries"] = max(p["retries"], -(-p["interval"] // 20) + 2)
            cfg["CONFIG_EXAMPLE_ESPNOW_WAKE_WINDOW_MS"] = p["window"]
            cfg["CONFIG_EXAMPLE_ESPNOW_WAKE_INTERVAL_MS"] = p["interval"]
            cfg["CONFIG_TBFT_ESPNOW_TX_RETRY_DELAY_MS"] = 20
        cfg["CONFIG_TBFT_ESPNOW_TX_RETRIES"] = p["retries"]
        lines += [f"{k}={v}" for k, v in cfg.items()]
    else:
        p["ps"] = base.replace("udp_ps", "")
        if kw.get("listen"):
            p["listen"] = kw["listen"]
            lines.append(f"CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL={p['listen']}")
    if p["pm"]:
        lines += PM
    return p, lines


def plan(stage, rate, listen):
    pts = []
    if stage == 1:
        for r in RATES:
            pts.append(point(f"en_r{r}_t3_pm0", "espnow", "espnow", rate=r))
        for ps in ("none", "min", "max"):
            for pm in (False, True):
                pts.append(point(f"udp_{ps}_pm{int(pm)}", "udp", f"udp_ps{ps}", pm=pm))
    elif stage == 2:
        if rate is None:
            sys.exit("stage 2 needs --rate <best wifi_phy_rate_t value from stage 1>")
        for pm in (False, True):
            for t in (3, 8):
                pts.append(point(f"en_r{rate}_t{t}_pm{int(pm)}", "espnow", "espnow",
                                 rate=rate, retries=t, pm=pm))
            for w, i in ((50, 100), (20, 100)):   # retries derived from the interval
                pts.append(point(f"enps_r{rate}_w{w}i{i}_pm{int(pm)}", "espnow",
                                 "espnow_ps", rate=rate, pm=pm, window=w, interval=i))
    elif stage == 3:
        for l in ([listen] if listen else (1, 3, 10)):
            pts.append(point(f"udp_max_l{l}_pm1", "udp", "udp_psmax", pm=True, listen=l))
    return pts


def load():
    with open(MANIFEST) as f:
        return json.load(f)


def cmd_plan(a):
    os.makedirs(SWEEPS, exist_ok=True)
    man = json.load(open(MANIFEST)) if os.path.exists(MANIFEST) else {}
    for p, lines in plan(a.stage, a.rate, a.listen):
        with open(os.path.join(SWEEPS, p["id"] + ".overlay"), "w") as f:
            f.write(f"# sweep point {p['id']}\n" + "\n".join(lines) + "\n")
        man[p["id"]] = p
    json.dump(man, open(MANIFEST, "w"), indent=1)
    print(f"{len(man)} points in {MANIFEST}")


def cmd_list(_):
    for p in load().values():
        extra = {k: v for k, v in p.items() if k not in ("id", "transport", "base") and v not in (None, False)}
        print(f"{p['id']:34} base={p['base']:11} {extra}")


def cmd_build(a):
    man = load()
    ids = a.only or list(man)
    for pid in ids:
        p = man[pid]
        for role in a.roles:
            bd = os.path.join("build_sweep", f"{pid}_{role}")
            env = dict(os.environ, EXTRA=os.path.join("sweeps", pid + ".overlay"), BUILD_DIR=bd)
            print(f"== build {pid} {role} -> {bd}", flush=True)
            r = subprocess.run(["./build_variant.sh", p["base"], role, "build"],
                               cwd=ROOT, env=env, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-1500:], r.stderr[-1500:])
                sys.exit(f"build failed: {pid} {role}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(required=True)
    p = sub.add_parser("plan"); p.add_argument("--stage", type=int, default=1)
    p.add_argument("--rate", type=int); p.add_argument("--listen", type=int); p.set_defaults(f=cmd_plan)
    p = sub.add_parser("list"); p.set_defaults(f=cmd_list)
    p = sub.add_parser("build"); p.add_argument("--only", nargs="*")
    p.add_argument("--roles", nargs="*", default=["replica", "client"]); p.set_defaults(f=cmd_build)
    a = ap.parse_args(); a.f(a)


if __name__ == "__main__":
    main()
