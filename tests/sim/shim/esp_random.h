#pragma once

#include <stdint.h>
#include <stddef.h>

/* Seeded PRNG (env SIM_SEED, node id and boot incarnation), so a rebooted
 * node draws different session keys, as the hardware RNG would. */
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);
