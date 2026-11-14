/*
 * Simulator runtime API (not part of ESP-IDF).  Used by the shim sources,
 * the sim mains and the principal stub.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Identity of this simulated board: used for log prefixes, PRNG seeding
 * (SIM_SEED ^ node id ^ SIM_INCARNATION) and the fake MAC address. */
void        sim_rt_set_node(int node_id, const char *role);
int         sim_rt_node_id(void);
int         sim_rt_incarnation(void);

/* PRNG streams (splitmix64/xoshiro256**), seeded per node and boot. */
enum { SIM_RNG_CRYPTO = 0, SIM_RNG_NET = 1, SIM_RNG_COUNT };
uint64_t    sim_rt_rand64(int stream);
double      sim_rt_rand_unit(int stream);         /* [0, 1) */

/* Times. */
int64_t     sim_rt_boot_us(void);   /* = esp_timer_get_time()            */
double      sim_rt_run_s(void);     /* seconds since SIM_T0_NS (run start) */

/* Task lock ("GIL").  Every task created with xTaskCreate() holds it while
 * running and drops it only while blocked in vTaskDelay(), select(),
 * xEventGroupWaitBits(), xSemaphoreTake() or xQueue*().  A monitor thread
 * that takes it therefore sees the protocol state between two steps of the
 * replica task, never half-way through a handler.  Timer callbacks do NOT
 * take it (on hardware the esp_timer task preempts application tasks). */
void        sim_gil_lock(void);
void        sim_gil_unlock(void);
bool        sim_gil_held(void);
/* Drop the lock around a blocking host call made from a task. */
bool        sim_blocking_begin(void);        /* returns "was held" */
void        sim_blocking_end(bool was_held);

/* Always printed, with the standard log prefix; level letter 'S'. */
void        sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void        sim_log_flush(void);

/* Network fault-injection counters (shim_src/sim_net.c). */
typedef struct {
    uint64_t sent;          /* datagrams handed to the host stack     */
    uint64_t dropped_loss;  /* dropped by SIM_LOSS                    */
    uint64_t dropped_block; /* dropped by SIM_BLOCK (partition)       */
} sim_net_stats_t;
void        sim_net_get_stats(sim_net_stats_t *out);

/* Optional CPU-time model for the stubbed crypto (env SIM_SIGN_MS,
 * SIM_VERIFY_MS, SIM_ENC_MS, SIM_DEC_MS; default 0 = host speed).  Busy
 * waits while holding the task lock, like a CPU-bound RSA operation. */
void        sim_rt_busy_ms(int ms);
int         sim_rt_env_int(const char *name, int dflt);
double      sim_rt_env_double(const char *name, double dflt);
