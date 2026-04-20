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
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#include <string.h>

/* Upper-bound slack for future-dated timestamps.  A Byzantine peer holding
 * the in-key can otherwise send a single message with msg_time_us near
 * INT64_MAX, latching last_auth_time_us and silently rejecting every
 * subsequent legitimate message as "stale". */
#ifndef TBFT_ANTI_REPLAY_FUTURE_SLACK_US
#define TBFT_ANTI_REPLAY_FUTURE_SLACK_US  ((int64_t)60 * 1000 * 1000) /* 60 s */
#endif

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
    if (p->psa_hmac_in_id  != 0) { psa_destroy_key(p->psa_hmac_in_id);  }
    if (p->psa_hmac_out_id != 0) { psa_destroy_key(p->psa_hmac_out_id); }
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

/* Import a raw key into PSA and return the key ID; 0 on failure. */
static psa_key_id_t import_hmac_key(const tbft_hmac_key_t *key,
                                    psa_key_usage_t usage)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, usage);

    psa_key_id_t kid = 0;
    psa_status_t st = psa_import_key(&attr, key->bytes, sizeof(key->bytes), &kid);
    if (st != PSA_SUCCESS) {
        return 0;
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
        return (st == PSA_SUCCESS) ? 0 : (int)st;
    }

    /* Slow fallback: import-per-op */
    psa_key_id_t kid = import_hmac_key(&p->hmac_out_key, PSA_KEY_USAGE_SIGN_MESSAGE);
    if (kid == 0) return -1;
    psa_status_t st = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                      (const uint8_t *)msg, msg_len,
                                      mac->bytes, TBFT_HMAC_SIZE, &mac_len);
    psa_destroy_key(kid);
    return (st == PSA_SUCCESS) ? 0 : (int)st;
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
    psa_key_id_t kid = import_hmac_key(&p->hmac_in_key, PSA_KEY_USAGE_VERIFY_MESSAGE);
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
    if (!tbft_principal_verify_mac_in(p, msg, msg_len, mac)) {
        return false;
    }

    /* Anti-replay: the sender stamps each outgoing message with
     * esp_timer_get_time(), which is strictly monotonic within one boot.
     * We reject any authenticated message whose timestamp is <= the highest
     * timestamp we have already accepted from this principal.
     *
     * Because the MAC covers the timestamp field in the header, an attacker
     * cannot bump the timestamp of a captured packet without also forging
     * the MAC (which requires the session key).
     *
     * On peer reboot the peer's clock resets to 0.  A New_key handshake
     * installs a fresh session key via tbft_principal_set_in_key, which
     * zeros last_auth_time_us so replay protection resumes correctly
     * against the new key stream. */
    if (TBFT_ANTI_REPLAY_WINDOW_US > 0) {
        if (msg_time_us <= 0) {
            /* Sender timestamps are esp_timer_get_time() values which are
             * always > 0 after the first microsecond post-boot.  Reject
             * anything with a zero-or-negative stamp as malformed. */
            ESP_LOGW(TAG, "anti-replay: invalid timestamp %lld from id=%d",
                     (long long)msg_time_us, (int)p->id);
            return false;
        }
        /* Reject timestamps far in the future.  Without this bound a single
         * authenticated Byzantine message with msg_time_us ≈ INT64_MAX would
         * latch the watermark and silently reject every subsequent legitimate
         * message from this principal for ~292 000 years. */
        int64_t now_us = esp_timer_get_time();
        if (msg_time_us > now_us + TBFT_ANTI_REPLAY_FUTURE_SLACK_US) {
            ESP_LOGW(TAG, "anti-replay: future-dated timestamp from id=%d "
                     "(msg=%lld, now=%lld, slack=%lld)",
                     (int)p->id,
                     (long long)msg_time_us,
                     (long long)now_us,
                     (long long)TBFT_ANTI_REPLAY_FUTURE_SLACK_US);
            return false;
        }
        if (msg_time_us <= p->last_auth_time_us) {
            ESP_LOGW(TAG, "anti-replay: stale timestamp from id=%d "
                     "(msg=%lld, last=%lld)",
                     (int)p->id,
                     (long long)msg_time_us,
                     (long long)p->last_auth_time_us);
            return false;
        }
        p->last_auth_time_us = msg_time_us;
    }
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
    p->keys_fresh  = true;

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

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_set_key_algorithm(&attr, TBFT_KEY_WRAP_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);

    mbedtls_svc_key_id_t key_id;
    int rc = mbedtls_pk_import_into_psa(&p->pub_pk, &attr, &key_id);
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
    }
    return (st == PSA_SUCCESS) ? 0 : (int)st;
}
