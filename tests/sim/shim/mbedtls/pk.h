/*
 * mbedtls/pk.h subset.  The simulator has no RSA: a "key" is the SHA-256
 * fingerprint of the key file's bytes.  The orchestrator writes the same
 * bytes to pubN.key and privN.key, so tbft_libbyz.c's key-pair check
 * (mbedtls_pk_write_pubkey_der on both contexts) still detects a mismatched
 * pair.  See tests/sim/stub_principal.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct mbedtls_pk_context {
    int           loaded;           /* 1 once parsed */
    unsigned char fingerprint[32];  /* SHA-256 of the key file */
} mbedtls_pk_context;

typedef enum {
    MBEDTLS_MD_NONE = 0,
    MBEDTLS_MD_SHA256 = 9,
} mbedtls_md_type_t;

#define MBEDTLS_ERR_PK_BAD_INPUT_DATA     -0x3E80
#define MBEDTLS_ERR_PK_KEY_INVALID_FORMAT -0x3D00
#define MBEDTLS_ERR_ASN1_BUF_TOO_SMALL    -0x006C

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
/* Writes the fingerprint at the END of buf (like the real DER writer) and
 * returns its length, or a negative error. */
int  mbedtls_pk_write_pubkey_der(const mbedtls_pk_context *ctx,
                                 unsigned char *buf, size_t size);
/* Simulator helper used by stub_principal.c. */
int  sim_pk_parse(mbedtls_pk_context *ctx, const unsigned char *key,
                  size_t keylen);
