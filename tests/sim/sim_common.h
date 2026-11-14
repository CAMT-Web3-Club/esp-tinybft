/* Helpers shared by sim_replica.c and sim_client.c. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Counter example wire format (examples/counter/main/main.c), with an
 * optional 8-byte request token after the op byte.  The token is only used
 * by the simulator's checker (exactly-once / ordering analysis); the
 * replica's exec_cb reads the op byte exactly like the example does. */
typedef struct [[gnu::packed]] {
    uint8_t  op;      /* 0 = increment, 1 = read */
    uint64_t token;   /* (client incarnation << 32) | request number */
} sim_counter_req_t;

typedef struct [[gnu::packed]] {
    int32_t value;
    int8_t  status;
} sim_counter_rep_t;

/* Look node @p id up in a UDP config file: copies its IP into @p ip.
 * Returns 0 on success.  Also returns num_nodes / num_replicas (hostnames
 * starting with "node") when the pointers are non-null. */
int  sim_cfg_lookup(const char *path, int id, char *ip, size_t ip_len,
                    int *num_nodes, int *num_replicas);

/* "<dir of config>/priv<id>.key" */
void sim_default_priv_path(const char *config, int id, char *out, size_t len);

void sim_hex(const uint8_t *d, size_t n, char *out /* 2n+1 */);

/* Block SIGTERM/SIGINT in the calling thread (call before creating threads)
 * and wait for one of them for at most @p ms; returns the signal or 0. */
void sim_block_term_signals(void);
int  sim_wait_term_signal(int ms);

[[noreturn]] void sim_fatal_reboot(const char *what);
