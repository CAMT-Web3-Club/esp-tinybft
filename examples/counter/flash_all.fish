#!/usr/bin/env fish
# Flash the image in build/ to every connected board, in parallel.
#
# With "Role of this board: Automatic" (the default) one image fits all
# eight boards: each board finds its role in the node list at boot.
# Build first, in a shell where idf.py works:
#
#   idf.py build
#   ./flash_all.fish                              # every /dev/ttyACM* port
#   ./flash_all.fish /dev/ttyACM0 /dev/ttyACM3    # only these ports
#
# Each port gets the command that idf.py prints after a build
# (python -m esptool ... write-flash @flash_args, run in the build
# directory), so the boards do not race each other on one build directory.

set -l dir (dirname (realpath (status --current-filename)))
set -l build $dir/build
if not test -f $build/flash_args
    echo "no $build/flash_args: run idf.py build first"
    exit 1
end

set -l ports $argv
if test (count $ports) -eq 0
    set ports /dev/ttyACM*
end
if test (count $ports) -eq 0
    echo "no /dev/ttyACM* port found"
    exit 1
end

set -l py python3
if set -q IDF_PYTHON_ENV_PATH; and test -x $IDF_PYTHON_ENV_PATH/bin/python
    set py $IDF_PYTHON_ENV_PATH/bin/python
end

mkdir -p $build/flash_logs
cd $build
echo "flashing $build to: $ports"
for port in $ports
    set -l log flash_logs/(basename $port).log
    fish -c "$py -m esptool --chip esp32c3 -p $port -b 460800 --before default-reset --after hard-reset write-flash @flash_args >$log 2>&1
        and echo '[$port] OK'
        or echo '[$port] FAILED, see $build/$log'" &
end
wait
