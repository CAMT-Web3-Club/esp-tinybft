#!/usr/bin/env python3
"""Flash the right image (replica or client) to every connected board.

  flash.py <target> [--ports PORT ...] [--client-port PORT] [--macs PORT=MAC ...]
                    [--dry-run] [-j N]

<target> is a variant name (images in build_<variant>_<role>/), a sweep point
id (build_sweep/<id>_<role>/) or a directory prefix (<prefix>_<role>/).
Build first: ./build_variant.sh <variant> replica && ./build_variant.sh
<variant> client, or tools/sweep.py build --only <id>.

Which image a board gets is decided from its MAC address, looked up in
spiffs_image/config_espnow.txt: entries whose name starts with "node" get the
replica image, every other entry the client image (the same rule the library
uses).  The MAC is read with esptool, which resets the board into the
bootloader.  A board whose MAC is not in the config is reported and skipped.

  --client-port PORT   do not read MACs: PORT gets the client image, every
                       other port the replica image
  --macs PORT=MAC ...  do not read MACs from those ports, use the given ones
  --dry-run            show the plan, flash nothing

Each board receives bootloader, partition table, application and the SPIFFS
image (keys + config files), exactly what `idf.py flash` writes.

Boards running a power-managed image (sdkconfig.pm) stop answering on their
USB Serial/JTAG port while asleep: hold BOOT and press RESET to enter the
bootloader before flashing them again.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG = os.path.join(ROOT, "spiffs_image", "config_espnow.txt")
MAC_RE = re.compile(r"MAC:\s*((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})")


def python_exe():
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env and os.path.exists(os.path.join(env, "bin", "python")):
        return os.path.join(env, "bin", "python")
    return sys.executable


def load_nodes():
    """[(index, name, mac)] from config_espnow.txt (5 header lines, then nodes)."""
    lines = open(CONFIG).read().split("\n")
    count = int(lines[3])
    nodes = []
    for i, line in enumerate(lines[5:5 + count]):
        f = line.split()
        if len(f) < 2:
            sys.exit(f"{CONFIG}: bad node line {i}: {line!r}")
        nodes.append((i, f[0], f[1].lower()))
    return nodes


def role_of(name):
    return "replica" if name.startswith("node") else "client"


def find_build(target, role):
    for d in (f"build_{target}_{role}", os.path.join("build_sweep", f"{target}_{role}"),
              f"{target}_{role}"):
        p = d if os.path.isabs(d) else os.path.join(ROOT, d)
        if os.path.isfile(os.path.join(p, "flash_args")):
            return p
    return None


def read_mac(port):
    r = subprocess.run([python_exe(), "-m", "esptool", "--port", port, "read-mac"],
                       capture_output=True, text=True, timeout=90)
    m = MAC_RE.search(r.stdout)
    if r.returncode != 0 or not m:
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["no output"]
        return None, tail[0]
    return m.group(1).lower(), None


def flash(port, build_dir, baud):
    chip = "esp32c3"
    try:
        chip = json.load(open(os.path.join(build_dir, "flasher_args.json")))["extra_esptool_args"]["chip"]
    except (OSError, KeyError, ValueError):
        pass
    r = subprocess.run([python_exe(), "-m", "esptool", "--chip", chip, "--port", port,
                        "--baud", str(baud), "--before", "default-reset", "--after",
                        "hard-reset", "write-flash", "@flash_args"],
                       cwd=build_dir, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        return (r.stdout + r.stderr).strip().splitlines()[-3:]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target")
    ap.add_argument("--ports", nargs="*")
    ap.add_argument("--client-port")
    ap.add_argument("--macs", nargs="*", default=[], metavar="PORT=MAC")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    a = ap.parse_args()

    builds = {role: find_build(a.target, role) for role in ("replica", "client")}
    ports = a.ports or sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if a.client_port and a.client_port not in ports:
        ports.append(a.client_port)
    if not ports:
        sys.exit("no serial ports found (pass --ports)")
    given = dict(x.split("=", 1) for x in a.macs)
    by_mac = {mac: (i, name) for i, name, mac in load_nodes()}

    plan, bad = [], 0
    for port in ports:
        node = "?"
        if a.client_port:
            role, mac = ("client" if port == a.client_port else "replica"), "(not read)"
        else:
            mac = given.get(port, "").lower() or None
            err = None
            if not mac:
                mac, err = read_mac(port)
            if not mac:
                print(f"{port}: cannot read MAC ({err}) - skipped. If the board runs a "
                      "power-managed image, hold BOOT and press RESET, then retry.")
                bad += 1
                continue
            if mac not in by_mac:
                print(f"{port}: MAC {mac} is not in spiffs_image/config_espnow.txt - skipped")
                bad += 1
                continue
            idx, name = by_mac[mac]
            node, role = f"{name} (id {idx})", role_of(name)
        if not builds[role]:
            sys.exit(f"no {role} image for '{a.target}': expected build_{a.target}_{role}/ or "
                     f"build_sweep/{a.target}_{role}/ with flash_args - build it first")
        plan.append((port, mac, node, role, builds[role]))

    print(f"{'port':14} {'mac':18} {'node':16} {'role':8} image")
    for port, mac, node, role, bd in plan:
        print(f"{port:14} {mac:18} {node:16} {role:8} {os.path.relpath(bd, ROOT)}")
    n_client = sum(1 for p in plan if p[3] == "client")
    if n_client != 1:
        print(f"note: {n_client} client board(s) in this plan (a full cluster has exactly 1)")
    if a.dry_run:
        print("dry run: nothing flashed")
        sys.exit(1 if bad else 0)

    with cf.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = {ex.submit(flash, port, bd, a.baud): (port, role) for port, _, _, role, bd in plan}
        for fut in cf.as_completed(futs):
            port, role = futs[fut]
            err = fut.result()
            if err:
                bad += 1
                print(f"{port}: FAILED ({role})\n    " + "\n    ".join(err))
            else:
                print(f"{port}: ok ({role})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
