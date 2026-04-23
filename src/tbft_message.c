/**
 * @file tbft_message.c
 * @brief Message utilities — SHA-256 digest computation and constant-time comparison.
 *
 * Provides cryptographic hashing (via PSA Crypto API) for:
 *   - Request set digest in Pre-prepare messages
 *   - State block digests in the partition tree
 *   - General message integrity checks
 *
 * The digest comparison uses a constant-time XOR fold to prevent timing
 * side-channel attacks on digest equality checks.
 */

#include "tbft_message.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include <string.h>

static const char *TAG = "tbft_msg";

void tbft_msg_digest(const void *data, size_t len, tbft_digest_t *out)
{
    /* M1 FIX: Ensure PSA crypto subsystem is initialized before any hash
     * operation. Without this, if tbft_msg_digest is called before any
     * principal has been initialized (e.g., during early state setup),
     * psa_hash_compute fails silently and produces a zero digest,
     * corrupting state block digests and partition tree nodes. */
    extern void tbft_principal_ensure_psa(void);
    tbft_principal_ensure_psa();

    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256,
                                       (const uint8_t *)data, len,
                                       out->bytes, TBFT_DIGEST_SIZE,
                                       &hash_len);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)st);
        memset(out->bytes, 0, TBFT_DIGEST_SIZE);
    }
}

bool tbft_digest_equal(const tbft_digest_t *a, const tbft_digest_t *b)
{
    /* Constant-time comparison to avoid timing side-channels */
    uint8_t diff = 0;
    for (int i = 0; i < TBFT_DIGEST_SIZE; i++) {
        diff |= a->bytes[i] ^ b->bytes[i];
    }
    return diff == 0;
}
