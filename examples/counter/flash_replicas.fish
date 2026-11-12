#!/usr/bin/env fish
# Flash every connected board with the image that matches its role (replica
# or client), decided from the board's MAC address.  Wrapper around
# tools/flash.py, kept under the old name.
#
#   ./flash_replicas.fish <variant | sweep point id> [--dry-run] [--ports ...] [--client-port ...]
#
# Build first:  ./build_variant.sh <variant> replica; ./build_variant.sh <variant> client
#
# The old behaviour (idf.py build + flash of the default build directory) is
# gone on purpose: the default sdkconfig no longer selects role or transport,
# so it could flash the wrong image to the replicas.

if test (count $argv) -lt 1
    echo "usage: ./flash_replicas.fish <variant|sweep-point> [--dry-run] [--ports ...] [--client-port ...]"
    echo "build first: ./build_variant.sh <variant> replica; ./build_variant.sh <variant> client"
    exit 2
end

set -l dir (dirname (status --current-filename))
set -l py python3
if set -q IDF_PYTHON_ENV_PATH; and test -x $IDF_PYTHON_ENV_PATH/bin/python
    set py $IDF_PYTHON_ENV_PATH/bin/python
end

exec $py $dir/tools/flash.py $argv
