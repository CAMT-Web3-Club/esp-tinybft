#include "tbft_principal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <string.h>

static const char *TAG = "tbft_principal";

/* --------------------------------------------------------------------------
 * Module-level PSA init (idempotent; called once before first crypto op)
 * -------------------------------------------------------------------------- */

static bool s_psa_init = false;

static void ensure_psa_init(void)
{
    if (!s_psa_init) {
        psa_status_t st = psa_crypto_init();
        if (st == PSA_SUCCESS) {
            s_psa_init = true;
        } else {
            ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        }
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_principal_init(tbft_principal_t *p, tbft_node_id_t id,
                         const tbft_addr_t *addr)
{
    memset(p, 0, sizeof(*p));
    p->id   = id;
    p->addr = *addr;
    mbedtls_pk_init(&p->pub_pk);
    mbedtls_pk_init(&p->priv_pk);
    ensure_psa_init();
}

int tbft_principal_load_pub_key(tbft_principal_t *p,
                                const uint8_t *der, size_t der_len)
{
    int ret = mbedtls_pk_parse_public_key(&p->pub_pk, der, der_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse_public_key failed: -0x%04x", (unsigned)(-ret));
    }
    return ret;
}

int tbft_principal_load_priv_key(tbft_principal_t *p,
                                 const uint8_t *der, size_t der_len)
{
    /* MbedTLS 4.x: no f_rng/p_rng in pk_parse_key */
    int ret = mbedtls_pk_parse_key(&p->priv_pk, der, der_len, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse_key failed: -0x%04x", (unsigned)(-ret));
        return ret;
    }
    p->has_priv_key = true;
    return 0;
}

void tbft_principal_free(tbft_principal_t *p)
{
    mbedtls_pk_free(&p->pub_pk);
    mbedtls_pk_free(&p->priv_pk);
    memset(p, 0, sizeof(*p));
}

/* --------------------------------------------------------------------------
 * HMAC helpers (PSA Crypto — mbedtls/md.h HMAC API is private in 4.x)
 * -------------------------------------------------------------------------- */

static psa_status_t hmac_sha256_psa(const tbft_hmac_key_t *key,
                                     const void *msg, size_t msg_len,
                                     tbft_mac_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);

    psa_key_id_t key_id;
    psa_status_t st = psa_import_key(&attr, key->bytes, sizeof(key->bytes), &key_id);
    if (st != PSA_SUCCESS) return st;

    size_t mac_len = 0;
    st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                         (const uint8_t *)msg, msg_len,
                         out->bytes, TBFT_HMAC_SIZE, &mac_len);
    psa_destroy_key(key_id);
    return st;
}

int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac)
{
    psa_status_t st = hmac_sha256_psa(&p->hmac_out_key, msg, msg_len, mac);
    return (st == PSA_SUCCESS) ? 0 : (int)st;
}

bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac)
{
    tbft_mac_t expected;
    if (hmac_sha256_psa(&p->hmac_in_key, msg, msg_len, &expected) != PSA_SUCCESS) {
        return false;
    }
    uint8_t diff = 0;
    for (int i = 0; i < TBFT_HMAC_SIZE; i++) {
        diff |= expected.bytes[i] ^ mac->bytes[i];
    }
    return diff == 0;
}

bool tbft_principal_verify_mac_in_with_replay_check(const tbft_principal_t *p,
                                                     const void *msg, size_t msg_len,
                                                     const tbft_mac_t *mac,
                                                     int64_t msg_time_us)
{
    if (!tbft_principal_verify_mac_in(p, msg, msg_len, mac)) {
        return false;
    }
    if (msg_time_us <= 0) {
        return true;
    }
    if (p->last_auth_time_us > 0 &&
        msg_time_us <= p->last_auth_time_us) {
        return false;
    }
    return true;
}

void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    p->hmac_in_key = *key;
    p->keys_fresh  = true;
}

void tbft_principal_set_out_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    p->hmac_out_key = *key;
}

/* --------------------------------------------------------------------------
 * RSA signature path
 * MbedTLS 4.x: pk_sign/pk_verify no longer take f_rng/p_rng parameters.
 * SHA-256 hashing uses PSA (mbedtls_sha256() was removed in 4.x).
 * -------------------------------------------------------------------------- */

int tbft_principal_sign(tbft_principal_t *p,
                        const void *msg, size_t msg_len,
                        tbft_sig_t *sig)
{
    if (!p->has_priv_key) {
        ESP_LOGE(TAG, "sign: no private key loaded for id=%d", p->id);
        return -1;
    }

    uint8_t hash[TBFT_DIGEST_SIZE];
    size_t  hash_len = 0;
    psa_status_t pst = psa_hash_compute(PSA_ALG_SHA_256,
                                        (const uint8_t *)msg, msg_len,
                                        hash, sizeof(hash), &hash_len);
    if (pst != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)pst);
        return (int)pst;
    }

    size_t sig_len = 0;
    /* MbedTLS 4.x pk_sign: no rng callback */
    int ret = mbedtls_pk_sign(&p->priv_pk, MBEDTLS_MD_SHA256,
                              hash, sizeof(hash),
                              sig->bytes, TBFT_SIG_SIZE, &sig_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_sign failed: -0x%04x", (unsigned)(-ret));
    }
    return ret;
}

bool tbft_principal_verify_sig(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               const tbft_sig_t *sig)
{
    uint8_t hash[TBFT_DIGEST_SIZE];
    size_t  hash_len = 0;
    psa_status_t pst = psa_hash_compute(PSA_ALG_SHA_256,
                                        (const uint8_t *)msg, msg_len,
                                        hash, sizeof(hash), &hash_len);
    if (pst != PSA_SUCCESS) return false;

    int ret = mbedtls_pk_verify((mbedtls_pk_context *)&p->pub_pk,
                                MBEDTLS_MD_SHA256,
                                hash, sizeof(hash),
                                sig->bytes, TBFT_SIG_SIZE);
    return ret == 0;
}

/* --------------------------------------------------------------------------
 * Session key rotation
 * mbedtls_pk_encrypt / mbedtls_pk_decrypt removed in MbedTLS 4.x.
 * We import the pk context into a transient PSA key for PKCS#1 v1.5.
 * -------------------------------------------------------------------------- */

int tbft_principal_encrypt_new_key(tbft_principal_t *p,
                                   const tbft_hmac_key_t *new_out_key,
                                   uint8_t *enc_buf, size_t enc_buf_len,
                                   size_t *out_enc_len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_PKCS1V15_CRYPT);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);

    mbedtls_svc_key_id_t key_id;
    int rc = mbedtls_pk_import_into_psa(&p->pub_pk, &attr, &key_id);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_import_into_psa (enc) failed: -0x%04x", (unsigned)(-rc));
        return rc;
    }

    psa_status_t st = psa_asymmetric_encrypt(key_id, PSA_ALG_RSA_PKCS1V15_CRYPT,
                                             new_out_key->bytes, sizeof(new_out_key->bytes),
                                             NULL, 0,
                                             enc_buf, enc_buf_len, out_enc_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_asymmetric_encrypt failed: %d", (int)st);
    }
    return (st == PSA_SUCCESS) ? 0 : (int)st;
}

int tbft_principal_decrypt_new_key(tbft_principal_t *p,
                                   const uint8_t *enc_buf, size_t enc_len,
                                   tbft_hmac_key_t *new_in_key)
{
    if (!p->has_priv_key) {
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_PKCS1V15_CRYPT);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);

    mbedtls_svc_key_id_t key_id;
    int rc = mbedtls_pk_import_into_psa(&p->priv_pk, &attr, &key_id);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_import_into_psa (dec) failed: -0x%04x", (unsigned)(-rc));
        return rc;
    }

    size_t out_len = 0;
    psa_status_t st = psa_asymmetric_decrypt(key_id, PSA_ALG_RSA_PKCS1V15_CRYPT,
                                             enc_buf, enc_len,
                                             NULL, 0,
                                             new_in_key->bytes, sizeof(new_in_key->bytes),
                                             &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_asymmetric_decrypt failed: %d", (int)st);
    }
    return (st == PSA_SUCCESS) ? 0 : (int)st;
}
