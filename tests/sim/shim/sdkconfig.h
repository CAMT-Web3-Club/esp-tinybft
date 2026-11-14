/*
 * Host-simulator sdkconfig.h.
 *
 * Values mirror examples/counter/sdkconfig.defaults plus the defaults of the
 * component Kconfig.projbuild, with the transport forced to UDP (unicast).
 * Every value can be overridden from the compiler command line, e.g.
 *   make EXTRA_CFLAGS=-DCONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS=5000
 *
 * As in a real sdkconfig.h, a disabled bool option is simply not defined.
 */
#pragma once

#define CONFIG_IDF_TARGET            "esp32c3"
#define CONFIG_IDF_TARGET_ESP32C3    1
#define IDF_VER                      "sim-host"

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ           1000
#endif

/* ---- TinyBFT: transport ------------------------------------------------ */
#define CONFIG_TBFT_TRANSPORT_UDP    1
#ifndef SIM_ENABLE_MULTICAST
#define CONFIG_TBFT_DISABLE_MULTICAST 1
#endif

/* ---- TinyBFT: protocol sizes ------------------------------------------- */
#ifndef CONFIG_TBFT_MAX_MESSAGE_SIZE
#define CONFIG_TBFT_MAX_MESSAGE_SIZE      2048
#endif
#ifndef CONFIG_TBFT_MAX_REPLY_SIZE
#define CONFIG_TBFT_MAX_REPLY_SIZE        900
#endif
#ifndef CONFIG_TBFT_BLOCK_SIZE
#define CONFIG_TBFT_BLOCK_SIZE            1024
#endif
#ifndef CONFIG_TBFT_MAX_NUM_REPLICAS
#define CONFIG_TBFT_MAX_NUM_REPLICAS      7
#endif
#ifndef CONFIG_TBFT_WINDOW_SIZE
#define CONFIG_TBFT_WINDOW_SIZE           4
#endif
#ifndef CONFIG_TBFT_CHECKPOINT_INTERVAL
#define CONFIG_TBFT_CHECKPOINT_INTERVAL   2
#endif
#ifndef CONFIG_TBFT_MAX_NUM_CLIENTS
#define CONFIG_TBFT_MAX_NUM_CLIENTS       1
#endif
#ifndef CONFIG_TBFT_MAX_STATE_BLOCKS
#define CONFIG_TBFT_MAX_STATE_BLOCKS      8
#endif
#ifndef CONFIG_TBFT_RQUEUE_MAX
#define CONFIG_TBFT_RQUEUE_MAX            4
#endif
#ifndef CONFIG_TBFT_NDET_BUF_SIZE
#define CONFIG_TBFT_NDET_BUF_SIZE         256
#endif
#ifndef CONFIG_TBFT_P_LEVELS
#define CONFIG_TBFT_P_LEVELS              2
#endif

/* ---- TinyBFT: timeouts (config-file values override the first three) --- */
#ifndef CONFIG_TBFT_VIEW_CHANGE_TIMEOUT_US
#define CONFIG_TBFT_VIEW_CHANGE_TIMEOUT_US 15000000
#endif
#ifndef CONFIG_TBFT_STATUS_TIMEOUT_US
#define CONFIG_TBFT_STATUS_TIMEOUT_US     1000000
#endif
#ifndef CONFIG_TBFT_RECOVERY_TIMEOUT_US
#define CONFIG_TBFT_RECOVERY_TIMEOUT_US   2500000
#endif
#ifndef CONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS
#define CONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS 20000
#endif
#ifndef CONFIG_TBFT_CLIENT_RETRANSMIT_MS
#define CONFIG_TBFT_CLIENT_RETRANSMIT_MS  5000
#endif
#ifndef CONFIG_TBFT_ANTI_REPLAY_WINDOW_MS
#define CONFIG_TBFT_ANTI_REPLAY_WINDOW_MS 30000
#endif
#ifndef CONFIG_TBFT_IDLE_WAIT_MS
#define CONFIG_TBFT_IDLE_WAIT_MS          500
#endif

/* ---- TinyBFT: statistics ------------------------------------------------ */
#ifndef SIM_NO_PRINT_STATS
#define CONFIG_TBFT_PRINT_STATS           1
#endif

/* ---- ESP-IDF system options the sources look at ------------------------- */
/* Task watchdog: enabled by default in ESP-IDF; the counter example sets a
 * 10 s timeout and no panic (ESP_TASK_WDT_PANIC is off by default). */
#ifndef SIM_NO_TASK_WDT
#define CONFIG_ESP_TASK_WDT_EN            1
#define CONFIG_ESP_TASK_WDT_INIT          1
#endif
#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S     10
#endif
#define CONFIG_LOG_DEFAULT_LEVEL          2   /* WARN, as in sdkconfig.defaults */
#define CONFIG_LWIP_UDP_RECVMBOX_SIZE     16
