#!/usr/bin/env python3
"""Plan and build the parameter sweep for the best-vs-best comparison.

  sweep.py plan  --stage N [--rates R ...]      write overlays + manifest
  sweep.py list                                  show planned points
  sweep.py build [--only ID ...] [--roles replica client] [--dry-run]

Only radio modes are swept.  No point uses CPU power management (light sleep
or frequency scaling): sdkconfig.pm is never applied here.

Each point is a base variant plus an overlay (sweeps/<id>.overlay); images go
to build_sweep/<id>_<role>/.  Run inside an activated IDF shell (idf.py on
PATH).  Flash with tools/flash.py <id>; measuring stays manual (see README).

Every knob below is an ESP-IDF feature, checked against the ESP-IDF v6.1 docs
and sources:
  ESP-NOW  station interface, per-peer PHY rate (esp_now_set_peer_rate_config),
           unicast or broadcast, receive duty cycle (esp_now_set_wake_window +
           esp_wifi_connectionless_module_set_wake_interval)
  UDP      Wi-Fi station power-save type (WIFI_PS_NONE / MIN_MODEM / MAX_MODEM),
           listen interval, minimum active time, unicast or IP multicast

Stages (plan them in order; each call adds to the manifest)
  1  always-on screen: ESP-NOW at every PHY rate + UDP PS_NONE.
     Energy is dominated by the idle radio here, so pick the ESP-NOW rate by
     success rate and latency (analyze.py prints a suggestion), not by req/J.
  2  ESP-NOW power saving at --rates (1-2 values from stage 1): receive duty
     cycle with the window/interval of the ESP-IDF example (50/100 ms), of
     the ESP-NOW bridge in Espressif's coin-cell button application note
     (110/200 ms) and one lower duty cycle (50/200 ms), plus an always-on
     point with more retries.
  3  UDP power saving: PS_MIN, PS_MAX (listen interval 3 and 10) and a
     shorter Wi-Fi minimum-active time.
  4  no-ACK sends: ESP-NOW broadcast (at --rates, default the IDF rate) and
     UDP multicast.  Counted as candidates for each transport's best.
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SWEEPS = os.path.join(ROOT, "sweeps")
MANIFEST = os.path.join(SWEEPS, "manifest.json")

# wifi_phy_rate_t values accepted by TBFT_ESPNOW_PHY_RATE, slowest first
RATES = {0: "1M (IDF default)", 11: "6M", 16: "MCS0 6.5M", 9: "24M", 12: "54M", 23: "MCS7 65M"}

# (window, interval, retry delay) in ms.  The retry delay stays well below the
# window so an attempt cannot step over it; the retry span covers the interval.
# Intervals are multiples of 100 ms as esp_wifi.h recommends.
DUTY = (
    (50, 100, 20),     # defaults of ESP-IDF examples/wifi/espnow (ESPNOW_WAKE_WINDOW/INTERVAL)
    (110, 200, 50),    # esp-matter esp-now_bridge_light defaults; esp-now coin-cell
                       # button app note sends to it with attempts 50 ms apart
    (50, 200, 20),     # lower duty cycle; allowed by the API, not a reference setting
)


def espnow(pid, base="espnow", rate=0, retries=3, duty=None, bcast=False):
    p = dict(id=pid, transport="espnow", base=base, rate=rate, retries=retries,
             retry_delay=10, retry_span=0, window=None, interval=None, bcast=bcast)
    if duty:
        p["window"], p["interval"], p["retry_delay"] = duty
        p["retry_span"] = p["interval"] + 20
    cfg = {"CONFIG_TBFT_ESPNOW_PHY_RATE": rate,
           "CONFIG_TBFT_ESPNOW_TX_RETRIES": retries,
           "CONFIG_TBFT_ESPNOW_TX_RETRY_DELAY_MS": p["retry_delay"],
           "CONFIG_TBFT_ESPNOW_TX_RETRY_SPAN_MS": p["retry_span"]}
    if duty:
        cfg["CONFIG_EXAMPLE_ESPNOW_WAKE_WINDOW_MS"] = p["window"]
        cfg["CONFIG_EXAMPLE_ESPNOW_WAKE_INTERVAL_MS"] = p["interval"]
    return p, [f"{k}={v}" for k, v in cfg.items()]


def udp(pid, ps, listen=None, min_active=None, mcast=False):
    p = dict(id=pid, transport="udp", base=f"udp_ps{ps}", ps=ps,
             listen=listen, min_active=min_active, mcast=mcast)
    lines = []
    if listen:
        lines.append(f"CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL={listen}")
    if min_active:
        lines.append(f"CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME={min_active}")
    if mcast:
        lines.append("CONFIG_TBFT_DISABLE_MULTICAST=n")
    return p, lines


def plan(stage, rates):
    pts = []
    if stage == 1:
        for r in RATES:
            pts.append(espnow(f"en_r{r}", rate=r))
        pts.append(udp("udp_none", "none"))
    elif stage == 2:
        if not rates:
            sys.exit("stage 2 needs --rates <1-2 wifi_phy_rate_t values chosen from stage 1>")
        for r in rates:
            pts.append(espnow(f"en_r{r}_t8", rate=r, retries=8))
            for d in DUTY:
                pts.append(espnow(f"enps_r{r}_w{d[0]}i{d[1]}", "espnow_ps", rate=r, duty=d))
    elif stage == 3:
        pts.append(udp("udp_min", "min"))
        pts.append(udp("udp_max_l3", "max", listen=3))
        pts.append(udp("udp_max_l10", "max", listen=10))
        pts.append(udp("udp_min_a20", "min", min_active=20))
        pts.append(udp("udp_max_l3_a20", "max", listen=3, min_active=20))
    elif stage == 4:
        for r in (rates or [0]):
            pts.append(espnow(f"enb_r{r}", "espnow_bcast", rate=r, bcast=True))
        pts.append(udp("udp_none_mcast", "none", mcast=True))
        pts.append(udp("udp_min_mcast", "min", mcast=True))
    else:
        sys.exit("--stage must be 1, 2, 3 or 4")
    return pts


def load():
    if not os.path.exists(MANIFEST):
        sys.exit("no sweeps/manifest.json - run 'sweep.py plan --stage 1' first")
    with open(MANIFEST) as f:
        return json.load(f)


def cmd_plan(a):
    os.makedirs(SWEEPS, exist_ok=True)
    man = json.load(open(MANIFEST)) if os.path.exists(MANIFEST) else {}
    for r in a.rates or []:
        if r not in RATES:
            sys.exit(f"unknown rate {r}; choose from {sorted(RATES)}")
    for p, lines in plan(a.stage, a.rates):
        with open(os.path.join(SWEEPS, p["id"] + ".overlay"), "w") as f:
            f.write(f"# sweep point {p['id']}\n" + "".join(l + "\n" for l in lines))
        man[p["id"]] = p
    json.dump(man, open(MANIFEST, "w"), indent=1)
    print(f"{len(man)} points in {os.path.relpath(MANIFEST, ROOT)}")


def cmd_list(_):
    for p in load().values():
        extra = {k: v for k, v in p.items()
                 if k not in ("id", "transport", "base") and v not in (None, False, 0)}
        print(f"{p['id']:22} base={p['base']:13} {extra}")


def cmd_build(a):
    man = load()
    ids = a.only or list(man)
    for pid in ids:
        if pid not in man:
            sys.exit(f"unknown point {pid} (see 'sweep.py list')")
    for pid in ids:
        p = man[pid]
        for role in a.roles:
            bd = os.path.join("build_sweep", f"{pid}_{role}")
            extra = os.path.join("sweeps", pid + ".overlay")
            print(f"== build {pid} {role} -> {bd}  [{extra}]", flush=True)
            if a.dry_run:
                continue
            env = dict(os.environ, EXTRA=extra, BUILD_DIR=bd)
            r = subprocess.run(["./build_variant.sh", p["base"], role, "build"],
                               cwd=ROOT, env=env, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-2000:], r.stderr[-2000:])
                sys.exit(f"build failed: {pid} {role}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(required=True)
    p = sub.add_parser("plan")
    p.add_argument("--stage", type=int, required=True)
    p.add_argument("--rates", type=int, nargs="*")
    p.set_defaults(f=cmd_plan)
    p = sub.add_parser("list")
    p.set_defaults(f=cmd_list)
    p = sub.add_parser("build")
    p.add_argument("--only", nargs="*")
    p.add_argument("--roles", nargs="*", default=["replica", "client"], choices=["replica", "client"])
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(f=cmd_build)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
