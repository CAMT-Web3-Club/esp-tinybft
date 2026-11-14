/*
 * SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104) in plain C, behind the
 * PSA hash calls tbft_message.c and tbft_partition.c use.
 */
#include "psa/crypto.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sim_sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sim_sha256_init(sim_sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->state, iv, sizeof(iv));
    c->bitlen = 0;
    c->buflen = 0;
}

void sim_sha256_update(sim_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;
    if (c->buflen) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
    while (len >= 64) {
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = (uint32_t)len;
    }
}

void sim_sha256_final(sim_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bitlen = c->bitlen;
    uint8_t pad = 0x80;
    /* Do not let the padding bytes count towards the message length. */
    sim_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sim_sha256_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bitlen >> (56 - 8 * i));
    sim_sha256_update(c, lenbuf, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->state[i]);
    }
}

void sim_sha256(const void *data, size_t len, uint8_t out[32])
{
    sim_sha256_ctx c;
    sim_sha256_init(&c);
    sim_sha256_update(&c, data, len);
    sim_sha256_final(&c, out);
}

void sim_hmac_sha256(const uint8_t *key, size_t key_len,
                     const void *msg, size_t msg_len, uint8_t out[32])
{
    uint8_t k[64] = {0};
    if (key_len > 64) {
        sim_sha256(key, key_len, k);
    } else if (key_len) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    sim_sha256_ctx c;
    sim_sha256_init(&c);
    sim_sha256_update(&c, ipad, 64);
    sim_sha256_update(&c, msg, msg_len);
    sim_sha256_final(&c, inner);
    sim_sha256_init(&c);
    sim_sha256_update(&c, opad, 64);
    sim_sha256_update(&c, inner, 32);
    sim_sha256_final(&c, out);
}

/* ---- PSA wrappers ------------------------------------------------------- */

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_hash_compute(psa_algorithm_t alg,
                              const uint8_t *input, size_t input_length,
                              uint8_t *hash, size_t hash_size,
                              size_t *hash_length)
{
    if (alg != PSA_ALG_SHA_256) return PSA_ERROR_NOT_SUPPORTED;
    if (hash_size < 32) return PSA_ERROR_BUFFER_TOO_SMALL;
    sim_sha256(input, input_length, hash);
    if (hash_length) *hash_length = 32;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    if (alg != PSA_ALG_SHA_256) return PSA_ERROR_NOT_SUPPORTED;
    if (op->alg != 0) return PSA_ERROR_BAD_STATE;
    op->alg = alg;
    sim_sha256_init(&op->ctx);
    return PSA_SUCCESS;
}

psa_status_t psa_hash_update(psa_hash_operation_t *op,
                             const uint8_t *input, size_t input_length)
{
    if (op->alg != PSA_ALG_SHA_256) return PSA_ERROR_BAD_STATE;
    sim_sha256_update(&op->ctx, input, input_length);
    return PSA_SUCCESS;
}

psa_status_t psa_hash_finish(psa_hash_operation_t *op,
                             uint8_t *hash, size_t hash_size,
                             size_t *hash_length)
{
    if (op->alg != PSA_ALG_SHA_256) return PSA_ERROR_BAD_STATE;
    if (hash_size < 32) {
        psa_hash_abort(op);
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    sim_sha256_final(&op->ctx, hash);
    if (hash_length) *hash_length = 32;
    op->alg = 0;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    memset(op, 0, sizeof(*op));
    return PSA_SUCCESS;
}
