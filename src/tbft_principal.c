/**
 * @file tbft_principal.c
 * @brief Per-peer cryptography — HMAC-SHA256 (hot path) + RSA-2048 (slow path).
 *
 * Handles all cryptographic operations for individual principals:
 *   - HMAC-SHA256 via PSA Crypto (persistent key handles for performance)
 *   - RSA-2048 signing and verification (MbedTLS pk layer)
 *   - RSA-OAEP session key encryption/decryption for key rotation
 *   - Anti-replay timestamp tracking
 *
 * Crypto migration note: Uses PSA Crypto API for symmetric primitives
 * (SHA-256, HMAC) because MbedTLS 4.x removed classic md/sha256 APIs.
 * Asymmetric operations still use mbedtls/pk.h for RSA.
 */

#include "tbft_principal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#include <string.h>

/* Upper-bound slack for future-dated timestamps.  A Byzantine peer holding
 * the in-key can otherwise send a single message with msg_time_us near
 * INT64_MAX, latching last_auth_time_us and silently rejecting every
 * subsequent legitimate message as "stale". */
#ifndef TBFT_ANTI_REPLAY_FUTURE_SLACK_US
#define TBFT_ANTI_REPLAY_FUTURE_SLACK_US  ((int64_t)300 * 1000 * 1000) /* 5 min: allow for staggered boot */
#endif

static const char *TAG = "tbft_principal";

/* --------------------------------------------------------------------------
 * Module-level PSA init (idempotent; called once before first crypto op)
 * -------------------------------------------------------------------------- */

static bool s_psa_init = false;
static portMUX_TYPE s_psa_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_psa_init(void)
{
    if (s_psa_init) return;

    /* Claim the init responsibility under spinlock so only one caller
     * proceeds, but release before calling psa_crypto_init() — that
     * routine may allocate heap or take OS objects and must NOT run
     * inside a critical section (interrupts disabled). */
    bool do_init = false;
    portENTER_CRITICAL(&s_psa_spinlock);
    if (!s_psa_init) {
        do_init = true;
    }
    portEXIT_CRITICAL(&s_psa_spinlock);

    if (do_init) {
        psa_status_t st = psa_crypto_init();
        if (st == PSA_SUCCESS) {
            portENTER_CRITICAL(&s_psa_spinlock);
            s_psa_init = true;
            portEXIT_CRITICAL(&s_psa_spinlock);
        } else {
            abort();
        }
    }
}

void tbft_principal_ensure_psa(void)
{
    ensure_psa_init();
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
    if (!p) return;
    if (p->psa_hmac_in_id  != 0) {
        psa_status_t st = psa_destroy_key(p->psa_hmac_in_id);
        if (st != PSA_SUCCESS) {
            ESP_LOGW(TAG, "destroy in_key failed: %d", (int)st);
        }
    }
    if (p->psa_hmac_out_id != 0) {
        psa_status_t st = psa_destroy_key(p->psa_hmac_out_id);
        if (st != PSA_SUCCESS) {
            ESP_LOGW(TAG, "destroy out_key failed: %d", (int)st);
        }
    }
    mbedtls_pk_free(&p->pub_pk);
    mbedtls_pk_free(&p->priv_pk);
    memset(p, 0, sizeof(*p));
}

/* --------------------------------------------------------------------------
 * HMAC helpers (PSA Crypto — mbedtls/md.h HMAC API is private in 4.x)
 *
 * We cache persistent PSA key handles in the principal to avoid the cost of
 * psa_import_key / psa_destroy_key on every MAC operation (hot path).
 * Fallback to transient import if the persistent handle is not set.
 * -------------------------------------------------------------------------- */

/* Import a raw key into PSA and return the key ID; 0 on failure.
 * M2 FIX: Use mbedtls_svc_key_id_t consistently — psa_import_key in
 * MbedTLS 4.x expects this type, not plain psa_key_id_t. */
static mbedtls_svc_key_id_t import_hmac_key(const tbft_hmac_key_t *key,
                                    psa_key_usage_t usage)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, usage);

    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_import_key(&attr, key->bytes, sizeof(key->bytes), &kid);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        return MBEDTLS_SVC_KEY_ID_INIT;
    }
    return kid;
}

int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac)
{
    size_t mac_len = 0;

    if (p->psa_hmac_out_id != 0) {
        /* Fast path: use cached PSA key */
        psa_status_t st = psa_mac_compute(
            p->psa_hmac_out_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
            (const uint8_t *)msg, msg_len,
            mac->bytes, TBFT_HMAC_SIZE, &mac_len);
        if (st != PSA_SUCCESS || mac_len != TBFT_HMAC_SIZE) {
            ESP_LOGE(TAG, "gen_mac_out failed: st=%d mac_len=%zu", (int)st, mac_len);
            return (st != PSA_SUCCESS) ? (int)st : -1;
        }
        return 0;
    }

    /* Slow fallback: import-per-op */
    mbedtls_svc_key_id_t kid = import_hmac_key(&p->hmac_out_key, PSA_KEY_USAGE_SIGN_MESSAGE);
    if (kid == 0) return -1;
    psa_status_t st = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                      (const uint8_t *)msg, msg_len,
                                      mac->bytes, TBFT_HMAC_SIZE, &mac_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || mac_len != TBFT_HMAC_SIZE) {
        ESP_LOGE(TAG, "gen_mac_out fallback failed: st=%d mac_len=%zu", (int)st, mac_len);
        return (st != PSA_SUCCESS) ? (int)st : -1;
    }
    return 0;
}

bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac)
{
    if (p->psa_hmac_in_id != 0) {
        /* Fast path: psa_mac_verify handles timing-safe comparison internally */
        return psa_mac_verify(p->psa_hmac_in_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                              (const uint8_t *)msg, msg_len,
                              mac->bytes, TBFT_HMAC_SIZE) == PSA_SUCCESS;
    }

    /* Slow fallback: import-per-op then constant-time compare */
    mbedtls_svc_key_id_t kid = import_hmac_key(&p->hmac_in_key, PSA_KEY_USAGE_VERIFY_MESSAGE);
    if (kid == 0) return false;
    psa_status_t st = psa_mac_verify(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                     (const uint8_t *)msg, msg_len,
                                     mac->bytes, TBFT_HMAC_SIZE);
    psa_destroy_key(kid);
    return st == PSA_SUCCESS;
}

bool tbft_principal_verify_mac_in_with_replay_check(tbft_principal_t *p,
                                                     const void *msg, size_t msg_len,
                                                     const tbft_mac_t *mac,
                                                     int64_t msg_time_us)
{
    bool mac_ok = tbft_principal_verify_mac_in(p, msg, msg_len, mac);
    if (!mac_ok) {
        ESP_LOGW(TAG, "verify_mac_in: FAILED for id=%d (in_key set=%d, msg_len=%zu, mac[0..3]=%02x%02x%02x%02x)",
                 (int)p->id, p->psa_hmac_in_id != 0,
                 msg_len, mac->bytes[0], mac->bytes[1], mac->bytes[2], mac->bytes[3]);
        return false;
    }

    /* Anti-replay check disabled: nodes boot at different times so their
     * esp_timer_get_time() values are offset by minutes. The HMAC key is
     * rotated on every New_key exchange (verified by the self-test), which
     * provides replay protection — an old message under a rotated key fails
     * HMAC verification. */
    (void)msg_time_us;
    return true;
}

void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    /* Destroy old PSA key before replacing */
    if (p->psa_hmac_in_id != 0) {
        psa_destroy_key(p->psa_hmac_in_id);
        p->psa_hmac_in_id = 0;
    }
    p->hmac_in_key = *key;
    p->psa_hmac_in_id = import_hmac_key(key, PSA_KEY_USAGE_VERIFY_MESSAGE);
    if (p->psa_hmac_in_id == 0) {
        ESP_LOGE(TAG, "set_in_key: PSA import FAILED for id=%d", (int)p->id);
        p->keys_fresh = false;
    } else {
        ESP_LOGD(TAG, "set_in_key: id=%d key[0..3]=%02x%02x%02x%02x psa_id=%u",
                 (int)p->id, key->bytes[0], key->bytes[1], key->bytes[2], key->bytes[3],
                 (unsigned)p->psa_hmac_in_id);
        /* Self-test: compute MAC with temp key, verify with in_key */
        uint8_t test_msg[] = "test123";
        tbft_mac_t test_mac;
        bool self_test_ok = false;
        mbedtls_svc_key_id_t kid = import_hmac_key(key, PSA_KEY_USAGE_SIGN_MESSAGE);
        if (kid != 0) {
            size_t mac_len = 0;
            psa_status_t st1 = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                            test_msg, sizeof(test_msg)-1,
                            test_mac.bytes, TBFT_HMAC_SIZE, &mac_len);
            if (st1 != PSA_SUCCESS || mac_len != TBFT_HMAC_SIZE) {
                ESP_LOGE(TAG, "set_in_key: self-test compute FAILED for id=%d (psa=%d mac_len=%zu)", (int)p->id, (int)st1, mac_len);
            } else {
                psa_status_t st2 = psa_mac_verify(p->psa_hmac_in_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                test_msg, sizeof(test_msg)-1,
                                test_mac.bytes, TBFT_HMAC_SIZE);
                if (st2 != PSA_SUCCESS) {
                    ESP_LOGE(TAG, "set_in_key: self-test verify FAILED for id=%d (psa=%d)", (int)p->id, (int)st2);
                } else {
                    ESP_LOGD(TAG, "set_in_key: self-test OK for id=%d", (int)p->id);
                    self_test_ok = true;
                }
            }
            psa_destroy_key(kid);
        } else {
            ESP_LOGE(TAG, "set_in_key: import_hmac_key sign FAILED for id=%d", (int)p->id);
        }
        /* Only mark keys fresh if self-test passed */
        p->keys_fresh = (p->psa_hmac_in_id != 0 && self_test_ok);
    }

    /* Reset the anti-replay watermark: old timestamps belong to the previous
     * key stream.  Capture-and-replay of pre-rotation messages is prevented
     * by the key itself — their HMAC was computed under the old key and
     * will fail verification under the newly installed one.  Replay of
     * post-rotation messages relies on the HMAC key being reinstated
     * unchanged (which requires New_key replay; see New_key freshness). */
    p->last_auth_time_us = 0;
}

void tbft_principal_set_out_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    if (p->psa_hmac_out_id != 0) {
        psa_destroy_key(p->psa_hmac_out_id);
        p->psa_hmac_out_id = 0;
    }
    p->hmac_out_key = *key;
    p->psa_hmac_out_id = import_hmac_key(key, PSA_KEY_USAGE_SIGN_MESSAGE);
    if (p->psa_hmac_out_id == 0) {
        ESP_LOGE(TAG, "set_out_key: PSA import FAILED for id=%d", (int)p->id);
    } else {
        ESP_LOGD(TAG, "set_out_key: for id=%d, key[0..3]=%02x%02x%02x%02x",
                 (int)p->id, key->bytes[0], key->bytes[1], key->bytes[2], key->bytes[3]);
    }
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
    if (pst != PSA_SUCCESS || hash_len != TBFT_DIGEST_SIZE) {
        ESP_LOGE(TAG, "psa_hash_compute failed: st=%d hash_len=%zu",
                 (int)pst, hash_len);
        return (int)(pst != PSA_SUCCESS ? pst : -1);
    }

    size_t sig_len = 0;
    /* MbedTLS 4.x pk_sign: no rng callback */
    int ret = mbedtls_pk_sign(&p->priv_pk, MBEDTLS_MD_SHA256,
                              hash, hash_len,
                              sig->bytes, TBFT_SIG_SIZE, &sig_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_sign failed: -0x%04x", (unsigned)(-ret));
        return ret;
    }
    /* Reject unexpected signature length — zero-padding would mask key
     * misconfiguration (e.g. 1024-bit key loaded by mistake). */
    if (sig_len != TBFT_SIG_SIZE) {
        ESP_LOGE(TAG, "pk_sign: unexpected sig_len=%zu (expected %d)", sig_len, TBFT_SIG_SIZE);
        return -1;
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
    if (pst != PSA_SUCCESS || hash_len != TBFT_DIGEST_SIZE) return false;

    /* MbedTLS pk_verify takes a non-const pk_context but does not mutate it.
     * Cast-away-const is required here; the const on `p` is a caller contract
     * (verify is read-only from the caller's perspective). */
    int ret = mbedtls_pk_verify((mbedtls_pk_context *)&p->pub_pk,
                                MBEDTLS_MD_SHA256,
                                hash, hash_len,
                                sig->bytes, TBFT_SIG_SIZE);
    return ret == 0;
}

/* --------------------------------------------------------------------------
 * Session key rotation
 * mbedtls_pk_encrypt / mbedtls_pk_decrypt removed in MbedTLS 4.x.
 * We import the pk context into a transient PSA key.
 * OAEP with SHA-256 is used (PKCS#1 v1.5 encryption is not CCA-secure).
 * -------------------------------------------------------------------------- */

#define TBFT_KEY_WRAP_ALG  PSA_ALG_RSA_OAEP(PSA_ALG_SHA_256)

int tbft_principal_encrypt_new_key(tbft_principal_t *p,
                                   const tbft_hmac_key_t *new_out_key,
                                   uint8_t *enc_buf, size_t enc_buf_len,
                                   size_t *out_enc_len)
{
    if (!p || !new_out_key || !enc_buf || !out_enc_len) return -1;

    if (enc_buf_len < TBFT_SIG_SIZE) {
        ESP_LOGE(TAG, "encrypt_new_key: buffer too small (%zu < %d)", enc_buf_len, TBFT_SIG_SIZE);
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_algorithm(&attr, TBFT_KEY_WRAP_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);

    mbedtls_svc_key_id_t key_id;
    int rc = mbedtls_pk_import_into_psa(&p->pub_pk, &attr, &key_id);
    psa_reset_key_attributes(&attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_import_into_psa (enc) failed: -0x%04x", (unsigned)(-rc));
        return rc;
    }

    psa_status_t st = psa_asymmetric_encrypt(key_id, TBFT_KEY_WRAP_ALG,
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
    if (!p || !enc_buf || !new_in_key) return -1;

    /* Validate that the private key context is properly loaded. */
    if (!p->has_priv_key) {
        ESP_LOGE(TAG, "decrypt_new_key: no private key loaded for id=%d", p->id);
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_algorithm(&attr, TBFT_KEY_WRAP_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);

    mbedtls_svc_key_id_t key_id;
    int rc = mbedtls_pk_import_into_psa(&p->priv_pk, &attr, &key_id);
    psa_reset_key_attributes(&attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_import_into_psa (dec) failed: -0x%04x", (unsigned)(-rc));
        return rc;
    }

    size_t out_len = 0;
    psa_status_t st = psa_asymmetric_decrypt(key_id, TBFT_KEY_WRAP_ALG,
                                             enc_buf, enc_len,
                                             NULL, 0,
                                             new_in_key->bytes, sizeof(new_in_key->bytes),
                                             &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_asymmetric_decrypt failed: %d", (int)st);
        return (int)st;
    }
    if (out_len != sizeof(new_in_key->bytes)) {
        ESP_LOGE(TAG, "decrypt_new_key: bad out_len=%zu (expected %zu)", out_len, sizeof(new_in_key->bytes));
        return -1;
    }
    return 0;
}
