#!/usr/bin/env fish

echo "=========================================="
echo "TinyBFT Replica Flashing Script (Fish Shell)"
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

for port in $replica_ports
    if test -e $port
        echo "------------------------------------------"
        echo "Flashing replica firmware to $port..."
        echo "------------------------------------------"
        idf.py -p $port flash
        if test $status -ne 0
            echo "Warning: Flashing failed on $port."
        else
            echo "Successfully flashed $port."
        end
    else
        echo "Skip: Port $port not found/connected."
    end
end

echo "=========================================="
echo "Flashing sequence finished."
echo "=========================================="
