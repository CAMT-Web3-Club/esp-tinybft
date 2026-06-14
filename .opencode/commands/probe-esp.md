---
description: Probe all ESP32-C3 boards and update esp-debugger .env
---
. ~/esp/esp-idf/export.sh
for port in /dev/ttyACM{0,1,2,3,4,5,6,7}; do
  esptool --port "$port" flash-id
done
# Match MACs to /home/phukrit7171/Development/esp-tinybft/examples/counter/spiffs_image/config_espnow.txt
# Write ports to /home/phukrit7171/Development/esp-debugger/.env
