/*
 * mbedtls pk / platform_util subset for the host simulator.  A key context
 * holds only the SHA-256 fingerprint of the key file (see shim/mbedtls/pk.h).
 */
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include <string.h>

void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) *p++ = 0;
}

void mbedtls_pk_init(mbedtls_pk_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void mbedtls_pk_free(mbedtls_pk_context *ctx)
{
    if (ctx) mbedtls_platform_zeroize(ctx, sizeof(*ctx));
}

int sim_pk_parse(mbedtls_pk_context *ctx, const unsigned char *key, size_t keylen)
{
    if (!ctx || !key || keylen == 0) return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    sim_sha256(key, keylen, ctx->fingerprint);
    ctx->loaded = 1;
    return 0;
}

int mbedtls_pk_write_pubkey_der(const mbedtls_pk_context *ctx,
                                unsigned char *buf, size_t size)
{
    if (!ctx || !ctx->loaded) return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    if (size < sizeof(ctx->fingerprint)) return MBEDTLS_ERR_ASN1_BUF_TOO_SMALL;
    memcpy(buf + size - sizeof(ctx->fingerprint), ctx->fingerprint,
           sizeof(ctx->fingerprint));
    return (int)sizeof(ctx->fingerprint);
}
