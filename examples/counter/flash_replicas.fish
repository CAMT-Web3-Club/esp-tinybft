#!/usr/bin/env fish

echo "=========================================="
echo "TinyBFT Replica Parallel Flashing Script"
echo "=========================================="

# Build the replica firmware first to ensure it's up to date
echo "Building replica firmware..."
idf.py build
if test $status -ne 0
    echo "Error: Build failed. Aborting flash."
    exit 1
end

# List of replica ports (/dev/ttyACM0 to /dev/ttyACM6)
# /dev/ttyACM7 is excluded as it is the client node
set replica_ports /dev/ttyACM3 /dev/ttyACM1 /dev/ttyACM2 /dev/ttyACM7 /dev/ttyACM4 /dev/ttyACM5 /dev/ttyACM6

# Create log directory inside build
mkdir -p build/flash_logs

set pids

for port in $replica_ports
    if test -e $port
        set -l port_name (basename $port)
        set -l log_file "build/flash_logs/$port_name.log"

        # Spawn a separate shell process for each device — no shared build system state
        fish -c "idf.py -p $port flash" > $log_file 2>&1 &
        set -a pids $last_pid

        echo "[$port] Flashing started (pid $last_pid)..."
    else
        echo "Skip: Port $port not found/connected."
    end
end

if test (count $pids) -gt 0
    echo "------------------------------------------"
    echo "Waiting for "(count $pids)" flashing jobs to complete..."
    echo "------------------------------------------"
    for pid in $pids
        wait $pid
        if test $status -eq 0
            echo "[pid $pid] SUCCESS"
        else
            echo "[pid $pid] FAILED — see build/flash_logs/ for details"
        end
    end
    echo "=========================================="
    echo "Parallel flashing sequence finished."
    echo "=========================================="
else
    echo "=========================================="
    echo "No connected replica ports found to flash."
    echo "=========================================="
end
