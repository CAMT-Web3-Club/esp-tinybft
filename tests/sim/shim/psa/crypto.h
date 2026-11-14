/*
 * PSA Crypto subset: SHA-256 only (a plain software implementation in
 * shim_src/sim_sha256.c).  Key types exist so tbft_principal.h compiles;
 * the MAC/RSA paths live in tests/sim/stub_principal.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t  psa_status_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_id_t;
typedef psa_key_id_t mbedtls_svc_key_id_t;
typedef uint32_t psa_key_usage_t;
typedef uint16_t psa_key_type_t;

#define PSA_SUCCESS                   ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR       ((psa_status_t)-132)
#define PSA_ERROR_NOT_SUPPORTED       ((psa_status_t)-134)
#define PSA_ERROR_INVALID_ARGUMENT    ((psa_status_t)-135)
#define PSA_ERROR_BUFFER_TOO_SMALL    ((psa_status_t)-138)
#define PSA_ERROR_BAD_STATE           ((psa_status_t)-137)
#define PSA_ERROR_INVALID_SIGNATURE   ((psa_status_t)-149)

#define PSA_ALG_SHA_256               ((psa_algorithm_t)0x02000009)
#define PSA_HASH_LENGTH(alg)          ((alg) == PSA_ALG_SHA_256 ? 32u : 0u)
#define MBEDTLS_SVC_KEY_ID_INIT       ((mbedtls_svc_key_id_t)0)

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} sim_sha256_ctx;

typedef struct psa_hash_operation_s {
    psa_algorithm_t alg;      /* 0 = inactive */
    sim_sha256_ctx  ctx;
} psa_hash_operation_t;

#define PSA_HASH_OPERATION_INIT {0}

psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(psa_algorithm_t alg,
                              const uint8_t *input, size_t input_length,
                              uint8_t *hash, size_t hash_size,
                              size_t *hash_length);
psa_status_t psa_hash_setup(psa_hash_operation_t *operation, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *operation,
                             const uint8_t *input, size_t input_length);
psa_status_t psa_hash_finish(psa_hash_operation_t *operation,
                             uint8_t *hash, size_t hash_size,
                             size_t *hash_length);
psa_status_t psa_hash_abort(psa_hash_operation_t *operation);

/* Direct helpers (simulator code). */
void sim_sha256_init(sim_sha256_ctx *c);
void sim_sha256_update(sim_sha256_ctx *c, const void *data, size_t len);
void sim_sha256_final(sim_sha256_ctx *c, uint8_t out[32]);
void sim_sha256(const void *data, size_t len, uint8_t out[32]);
void sim_hmac_sha256(const uint8_t *key, size_t key_len,
                     const void *msg, size_t msg_len, uint8_t out[32]);
