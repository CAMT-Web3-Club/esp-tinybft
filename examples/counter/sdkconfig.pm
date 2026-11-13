# NOT part of the best-vs-best comparison: that comparison uses radio modes
# only and no point of tools/sweep.py applies this file.
#
# CPU frequency scaling + automatic light sleep, following the ESP32-C3 table
# of the ESP-IDF v6.1 low-power guide ("Auto Light-sleep Mode").  Only helps
# when the radio can sleep (UDP PS_MIN/PS_MAX or espnow_ps); with an always-on
# radio the Wi-Fi driver holds a PM lock and nothing changes.
#
# In light sleep the USB Serial/JTAG port stops answering (the board may
# disappear from the host and need BOOT+RESET to be flashed again).  Take logs
# from UART0, or see sdkconfig.usblog.
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
# 1000 Hz tick with a 3-tick threshold: a 3 ms idle gap is enough to sleep
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
# The ESP32-C3 table in the guide leaves PM_SLP_IRAM_OPT / PM_RTOS_IDLE_OPT
# off; enabling them (as examples/wifi/power_save does) costs about 13 KB of
# RAM, which this example cannot spare next to the replica state.
# recommended light-sleep power-downs for ESP32-C3
CONFIG_PM_SLP_DISABLE_GPIO=y
CONFIG_ESP_PHY_MAC_BB_PD=y
CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y
