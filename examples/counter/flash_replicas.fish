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
set replica_ports /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2 /dev/ttyACM3 /dev/ttyACM4 /dev/ttyACM5 /dev/ttyACM6

# Create log directory inside build
mkdir -p build/flash_logs

set active_jobs 0

for port in $replica_ports
    if test -e $port
        set -l port_name (basename $port)
        set -l log_file "build/flash_logs/$port_name.log"

        # Run flashing in parallel
        begin
            echo "[$port] Flashing started..."
            if idf.py -p $port flash > $log_file 2>&1
                echo "[$port] SUCCESS: Flashed successfully."
            else
                echo "[$port] FAILED: Flashing failed. See $log_file for details."
            end
        end &

        set active_jobs (math $active_jobs + 1)
    else
        echo "Skip: Port $port not found/connected."
    end
end

if test $active_jobs -gt 0
    echo "------------------------------------------"
    echo "Waiting for $active_jobs flashing jobs to complete..."
    echo "------------------------------------------"
    wait
    echo "=========================================="
    echo "Parallel flashing sequence finished."
    echo "=========================================="
else
    echo "=========================================="
    echo "No connected replica ports found to flash."
    echo "=========================================="
end
