# CPU DFS + automatic light sleep, following Espressif's recommended ESP32-C3
# configuration (ESP-IDF v6.1 docs api-guides/low-power-mode, "Auto Light-sleep
# Mode", and examples/wifi/power_save).  Apply to every point of a comparison
# or to none.  Only helps when the radio can sleep: UDP PS_MIN/PS_MAX or the
# espnow_ps variant.  With an always-on radio the Wi-Fi driver holds a PM lock
# and neither light sleep nor frequency scaling happens.
#
# NOTE: in light sleep the USB Serial/JTAG port stops answering (the board may
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
