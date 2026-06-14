/**
 * @file tbft_principal.c
 * @brief Per-peer cryptography — HMAC-SHA256 + ECDSA P-256 + ECDH via PSA.
 *
 * All operations use persistent PSA key handles:
 *   - HMAC-SHA256 (hot path) via cached psa_hmac_in/out_id
 *   - ECDSA P-256 signing/verification (PKCS#1-v1.5-style deterministic)
 *   - ECDH P-256 key agreement for New_key exchange
 *
 * Zero RSA.  Zero mbedtls/pk.  Pure PSA Crypto.
 */

#include "tbft_principal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"
#include <string.h>

static const char *TAG = "tbft_principal";

#define ECDSA_ALG  PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

/* --------------------------------------------------------------------------
 * Module-level PSA init
 * -------------------------------------------------------------------------- */

static bool s_psa_init = false;
static portMUX_TYPE s_psa_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_psa_init(void)
{
    if (s_psa_init) return;
    bool do_init = false;
    portENTER_CRITICAL(&s_psa_spinlock);
    if (!s_psa_init) do_init = true;
    portEXIT_CRITICAL(&s_psa_spinlock);
    if (do_init) {
        psa_status_t st = psa_crypto_init();
        if (st == PSA_SUCCESS) {
            portENTER_CRITICAL(&s_psa_spinlock);
            s_psa_init = true;
            portEXIT_CRITICAL(&s_psa_spinlock);
        } else { abort(); }
    }
}

void tbft_principal_ensure_psa(void) { ensure_psa_init(); }

/* --------------------------------------------------------------------------
 * PSA key import helpers
 * -------------------------------------------------------------------------- */

/** Import raw private key bytes (32 bytes) for signing. */
static mbedtls_svc_key_id_t import_sign_key(const uint8_t *priv, size_t len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, ECDSA_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);

    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_import_key(&attr, priv, len, &kid);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "import sign key failed: %d", (int)st);
        return MBEDTLS_SVC_KEY_ID_INIT;
    }
    return kid;
}

/** Import raw private key bytes (32 bytes) for ECDH. */
static mbedtls_svc_key_id_t import_ecdh_key(const uint8_t *priv, size_t len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);

    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_import_key(&attr, priv, len, &kid);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "import ECDH key failed: %d", (int)st);
        return MBEDTLS_SVC_KEY_ID_INIT;
    }
    return kid;
}

/** Import uncompressed public key bytes (65 bytes: 0x04 || X || Y). */
static mbedtls_svc_key_id_t import_verify_key(const uint8_t *pub, size_t len)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, ECDSA_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);

    mbedtls_svc_key_id_t kid = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_import_key(&attr, pub, len, &kid);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "import verify key failed: %d", (int)st);
        return MBEDTLS_SVC_KEY_ID_INIT;
    }
    return kid;
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
    ensure_psa_init();
}

int tbft_principal_load_pub_key(tbft_principal_t *p,
                                const uint8_t *pub_bytes, size_t pub_len)
{
    if (pub_len != TBFT_ECDSA_PUB_SIZE) {
        ESP_LOGE(TAG, "load_pub_key: bad len %zu (expected %d)", pub_len, TBFT_ECDSA_PUB_SIZE);
        return -1;
    }
    if (p->pub_verify_id != 0) {
        psa_destroy_key(p->pub_verify_id);
        p->pub_verify_id = 0;
    }

    /* Cache the raw public key bytes for ECDH */
    memcpy(p->pub_key_bytes, pub_bytes, TBFT_ECDSA_PUB_SIZE);

    p->pub_verify_id = import_verify_key(pub_bytes, pub_len);
    return (p->pub_verify_id != 0) ? 0 : -1;
}

int tbft_principal_load_priv_key(tbft_principal_t *p,
                                 const uint8_t *priv_bytes, size_t priv_len)
{
    if (priv_len != TBFT_ECDSA_PRIV_SIZE) {
        ESP_LOGE(TAG, "load_priv_key: bad len %zu (expected %d)", priv_len, TBFT_ECDSA_PRIV_SIZE);
        return -1;
    }
    if (p->priv_sign_id != 0) { psa_destroy_key(p->priv_sign_id); p->priv_sign_id = 0; }
    if (p->priv_ecdh_id != 0) { psa_destroy_key(p->priv_ecdh_id); p->priv_ecdh_id = 0; }

    p->priv_sign_id = import_sign_key(priv_bytes, priv_len);
    if (p->priv_sign_id == 0) return -1;

    p->priv_ecdh_id = import_ecdh_key(priv_bytes, priv_len);
    if (p->priv_ecdh_id == 0) {
        psa_destroy_key(p->priv_sign_id);
        p->priv_sign_id = 0;
        return -1;
    }
    return 0;
}

void tbft_principal_free(tbft_principal_t *p)
{
    if (!p) return;
    if (p->psa_hmac_in_id      != 0) psa_destroy_key(p->psa_hmac_in_id);
    if (p->psa_hmac_in_id_prev != 0) psa_destroy_key(p->psa_hmac_in_id_prev);
    if (p->psa_hmac_out_id     != 0) psa_destroy_key(p->psa_hmac_out_id);
    if (p->psa_hmac_out_id_old != 0) psa_destroy_key(p->psa_hmac_out_id_old);  /* B-FIX */
    if (p->pub_verify_id       != 0) psa_destroy_key(p->pub_verify_id);
    if (p->priv_sign_id        != 0) psa_destroy_key(p->priv_sign_id);
    if (p->priv_ecdh_id        != 0) psa_destroy_key(p->priv_ecdh_id);
    memset(p, 0, sizeof(*p));
}

/* --------------------------------------------------------------------------
 * HMAC helpers (unchanged from v0.7.0)
 * -------------------------------------------------------------------------- */

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
    if (st != PSA_SUCCESS) return MBEDTLS_SVC_KEY_ID_INIT;
    return kid;
}

int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac)
{
    size_t mac_len = 0;
    /* B-FIX: During the out_key grace window (set_out_key was just called
     * for a rotation, but peers may not have processed New_key yet), sign
     * with the OLD key.  The receiver's in_key_prev fallback matches this.
     * Once the grace period expires, sign with the NEW key. */
    int64_t now = esp_timer_get_time();
    /* FB-FIX (v0.9.0 → v0.9.1): Drop the `psa_hmac_out_id_old != 0` clause.
     * On the FIRST call to tbft_principal_set_out_key, psa_hmac_out_id_old
     * is 0 (transferred from the never-imported psa_hmac_out_id), so the
     * old check would yield in_grace=false on the initial rotation.  This
     * caused the first PP to be signed with the NEW ECDH-derived out_key
     * while every receiver's hmac_in_key was still all-zeros from
     * tbft_principal_init — every PP failed MAC verification until the
     * 3s grace expired, by which time the cluster had already entered a
     * view-change storm.  The cold path (line 218) re-imports
     * hmac_out_key_old from raw bytes and is safe to use for the all-
     * zeros initial key, matching the receiver's all-zeros in_key. */
    bool in_grace = (p->out_key_old_expires_us > 0) &&
                    (now < p->out_key_old_expires_us);
    mbedtls_svc_key_id_t kid = in_grace ? p->psa_hmac_out_id_old : p->psa_hmac_out_id;
    if (kid != 0) {
        psa_status_t st = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
            (const uint8_t *)msg, msg_len, mac->bytes, TBFT_HMAC_SIZE, &mac_len);
        if (st != PSA_SUCCESS || mac_len != TBFT_HMAC_SIZE) return (st != PSA_SUCCESS) ? (int)st : -1;
        return 0;
    }
    /* Cold path: import the active key on demand (grace aware). */
    const tbft_hmac_key_t *src = in_grace ? &p->hmac_out_key_old : &p->hmac_out_key;
    kid = import_hmac_key(src, PSA_KEY_USAGE_SIGN_MESSAGE);
    if (kid == 0) return -1;
    psa_status_t st = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        (const uint8_t *)msg, msg_len, mac->bytes, TBFT_HMAC_SIZE, &mac_len);
    psa_destroy_key(kid);
    if (st != PSA_SUCCESS || mac_len != TBFT_HMAC_SIZE) return (st != PSA_SUCCESS) ? (int)st : -1;
    return 0;
}

bool tbft_principal_in_out_key_grace(const tbft_principal_t *p)
{
    if (!p) return false;
    int64_t now = esp_timer_get_time();
    /* FB-FIX: mirror of gen_mac_out fix — drop the psa_hmac_out_id_old != 0
     * clause.  On the first rotation, in_grace must be true so the cold
     * path imports the all-zeros hmac_out_key_old to match the receiver's
     * all-zeros hmac_in_key. */
    return (p->out_key_old_expires_us > 0) &&
           (now < p->out_key_old_expires_us);
}

void tbft_principal_commit_out_key(tbft_principal_t *p)
{
    if (!p) return;
    if (p->psa_hmac_out_id_old != 0) {
        psa_destroy_key(p->psa_hmac_out_id_old);
        p->psa_hmac_out_id_old = 0;
    }
    p->out_key_old_expires_us = 0;
    memset(&p->hmac_out_key_old, 0, sizeof(p->hmac_out_key_old));
    p->out_key_ack_received = false;
    memset(p->pending_out_key_nonce, 0, sizeof(p->pending_out_key_nonce));
}

bool tbft_principal_ack_out_key(tbft_principal_t *p, const uint8_t *nonce)
{
    if (!p || !nonce) return false;
    /* Match against the currently-pending nonce.  Stale ACKs from a
     * prior rotation (nonce already cleared) are silently ignored. */
    if (memcmp(p->pending_out_key_nonce, nonce, 32) != 0) return false;
    p->out_key_ack_received = true;
    return true;
}

const uint8_t *tbft_principal_pending_out_key_nonce(const tbft_principal_t *p)
{
    if (!p) return NULL;
    if (!p->out_key_ack_received &&
        p->pending_out_key_nonce[0] == 0 &&
        p->pending_out_key_nonce[31] == 0) {
        /* No rotation is pending */
        return NULL;
    }
    return p->pending_out_key_nonce;
}

bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac)
{
    if (p->psa_hmac_in_id != 0)
        return psa_mac_verify(p->psa_hmac_in_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
            (const uint8_t *)msg, msg_len, mac->bytes, TBFT_HMAC_SIZE) == PSA_SUCCESS;
    mbedtls_svc_key_id_t kid = import_hmac_key(&p->hmac_in_key, PSA_KEY_USAGE_VERIFY_MESSAGE);
    if (kid == 0) return false;
    psa_status_t st = psa_mac_verify(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        (const uint8_t *)msg, msg_len, mac->bytes, TBFT_HMAC_SIZE);
    psa_destroy_key(kid);
    return st == PSA_SUCCESS;
}

bool tbft_principal_verify_mac_in_with_replay_check(tbft_principal_t *p,
    const void *msg, size_t msg_len, const tbft_mac_t *mac, int64_t msg_time_us)
{
    /* Try current in_key first */
    if (tbft_principal_verify_mac_in(p, msg, msg_len, mac)) {
        (void)msg_time_us;
        return true;
    }

    /* Key rotation grace period: the sender may have signed this
     * message with the old key before processing our last New_key.
     * Try the previous in_key as a fallback.  Re-import from saved
     * bytes to avoid storing a PSA key ID (which would dangle after
     * psa_hmac_in_id is destroyed in the next set_in_key call). */
    {
        mbedtls_svc_key_id_t kid = import_hmac_key(&p->hmac_in_key_prev,
            PSA_KEY_USAGE_VERIFY_MESSAGE);
        if (kid != 0) {
            psa_status_t st = psa_mac_verify(kid,
                PSA_ALG_HMAC(PSA_ALG_SHA_256),
                (const uint8_t *)msg, msg_len, mac->bytes, TBFT_HMAC_SIZE);
            psa_destroy_key(kid);
            if (st == PSA_SUCCESS) {
                (void)msg_time_us;
                return true;
            }
        }
    }

    return false;
}

void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    /* Retain previous key bytes for rotation grace period fallback.
     * Do NOT store psa_hmac_in_id_prev — it would dangle after the
     * destroy below (the old psa_hmac_in_id and psa_hmac_in_id_prev
     * would point to the same PSA key object).  The fallback path in
     * verify_mac_in_with_replay_check re-imports from hmac_in_key_prev
     * bytes instead. */
    p->hmac_in_key_prev = p->hmac_in_key;

    if (p->psa_hmac_in_id != 0) { psa_destroy_key(p->psa_hmac_in_id); p->psa_hmac_in_id = 0; }
    p->hmac_in_key = *key;
    p->psa_hmac_in_id = import_hmac_key(key, PSA_KEY_USAGE_VERIFY_MESSAGE);
    if (p->psa_hmac_in_id == 0) {
        p->keys_fresh = false;
    } else {
        uint8_t test_msg[] = "test123";
        tbft_mac_t test_mac;
        bool ok = false;
        mbedtls_svc_key_id_t kid = import_hmac_key(key, PSA_KEY_USAGE_SIGN_MESSAGE);
        if (kid != 0) {
            size_t mac_len = 0;
            psa_status_t st = psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                test_msg, sizeof(test_msg)-1, test_mac.bytes, TBFT_HMAC_SIZE, &mac_len);
            if (st == PSA_SUCCESS && mac_len == TBFT_HMAC_SIZE) {
                ok = psa_mac_verify(p->psa_hmac_in_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                    test_msg, sizeof(test_msg)-1, test_mac.bytes, TBFT_HMAC_SIZE) == PSA_SUCCESS;
            }
            psa_destroy_key(kid);
        }
        p->keys_fresh = (p->psa_hmac_in_id != 0 && ok);
    }
    p->last_auth_time_us = 0;
}

void tbft_principal_set_out_key(tbft_principal_t *p,
                                  const tbft_hmac_key_t *key,
                                  const uint8_t *nonce)
{
    /* B-FIX: Save the previous out_key as the "old" key for the rotation
     * grace period.  gen_mac_out will sign with the old key until
     * commit_out_key is called.  The receiver's in_key_prev fallback
     * matches the old key, so verification succeeds during the grace
     * window.
     *
     * If we are already in a grace period (consecutive rotations without
     * commit), the previous "old" key is replaced with the current key.
     * The current key is the more recent of the two rotation sources and
     * is more likely to be what peers have already processed. */
    if (p->psa_hmac_out_id_old != 0) psa_destroy_key(p->psa_hmac_out_id_old);

    p->hmac_out_key_old = p->hmac_out_key;
    p->psa_hmac_out_id_old = p->psa_hmac_out_id;  /* transfer ownership */

    /* v0.9.2 ACK: Record the nonce this rotation is using.  Peers will
     * send New_key_ack with this nonce; we match ACKs against it and
     * commit per-peer when matched.  This replaces the time-based
     * grace deadline (15s in v0.9.1) with a real protocol. */
    if (nonce) {
        memcpy(p->pending_out_key_nonce, nonce, 32);
    } else {
        memset(p->pending_out_key_nonce, 0, sizeof(p->pending_out_key_nonce));
    }
    p->out_key_ack_received = false;

    /* v0.9.1 fallback: 15s grace deadline in case ACKs never arrive
     * (peer is down or slow).  The per-peer commit will still fire
     * after this timeout even without an ACK. */
    p->out_key_old_expires_us = esp_timer_get_time() + 15 * 1000 * 1000;

    /* Install new key as the "active" out_key.  gen_mac_out will use the
     * old key during the grace period (see in_grace check above). */
    p->hmac_out_key = *key;
    p->psa_hmac_out_id = import_hmac_key(key, PSA_KEY_USAGE_SIGN_MESSAGE);
}

/* --------------------------------------------------------------------------
 * ECDSA signature path — pure PSA
 * -------------------------------------------------------------------------- */

int tbft_principal_sign(tbft_principal_t *p,
                        const void *msg, size_t msg_len,
                        tbft_sig_t *sig)
{
    if (p->priv_sign_id == 0) {
        ESP_LOGE(TAG, "sign: no private key loaded for id=%d", p->id);
        return -1;
    }
    uint8_t hash[TBFT_DIGEST_SIZE];
    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256,
        (const uint8_t *)msg, msg_len, hash, sizeof(hash), &hash_len);
    if (st != PSA_SUCCESS || hash_len != TBFT_DIGEST_SIZE) return -1;

    size_t sig_len = TBFT_SIG_SIZE;
    memset(sig, 0, sizeof(tbft_sig_t));
    st = psa_sign_hash(p->priv_sign_id, ECDSA_ALG,
        hash, hash_len, sig->bytes, TBFT_SIG_SIZE, &sig_len);
    if (st != PSA_SUCCESS || sig_len != TBFT_SIG_SIZE) {
        ESP_LOGE(TAG, "psa_sign_hash failed: st=%d len=%zu", (int)st, sig_len);
        return -1;
    }
    return 0;
}

bool tbft_principal_verify_sig(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               const tbft_sig_t *sig)
{
    if (p->pub_verify_id == 0) return false;
    uint8_t hash[TBFT_DIGEST_SIZE];
    size_t hash_len = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256,
        (const uint8_t *)msg, msg_len, hash, sizeof(hash), &hash_len);
    if (st != PSA_SUCCESS || hash_len != TBFT_DIGEST_SIZE) return false;
    return psa_verify_hash(p->pub_verify_id, ECDSA_ALG,
        hash, hash_len, sig->bytes, TBFT_SIG_SIZE) == PSA_SUCCESS;
}

/* --------------------------------------------------------------------------
 * ECDH session key derivation (replaces RSA encrypt/decrypt)
 * -------------------------------------------------------------------------- */

int tbft_principal_ecdh_derive_key(const tbft_principal_t *p,
                                   const tbft_principal_t *peer,
                                   const uint8_t salt[32],
                                   tbft_hmac_key_t *out_key)
{
    if (!p || !peer || !salt || !out_key) return -1;
    if (p->priv_ecdh_id == 0) {
        ESP_LOGE(TAG, "ecdh_derive: no ECDH key loaded for id=%d", p->id);
        return -1;
    }

    /* ECDH: shared = priv * peer_pub */
    uint8_t shared[32];
    size_t shared_len = 0;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, p->priv_ecdh_id,
        peer->pub_key_bytes, TBFT_ECDSA_PUB_SIZE,
        shared, sizeof(shared), &shared_len);
    if (st != PSA_SUCCESS || shared_len != 32) {
        ESP_LOGE(TAG, "ecdh_derive: psa_raw_key_agreement failed: %d", (int)st);
        return -1;
    }

    /* HKDF-Extract: PRK = HMAC(salt, shared) — salt is the key, shared is the msg */
    uint8_t prk[TBFT_HMAC_SIZE];
    size_t prk_len = 0;

    /* Use salt as the HMAC key for the Extract step */
    psa_key_attributes_t salt_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&salt_attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&salt_attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&salt_attr, PSA_KEY_USAGE_SIGN_MESSAGE);

    mbedtls_svc_key_id_t salt_id = MBEDTLS_SVC_KEY_ID_INIT;
    st = psa_import_key(&salt_attr, salt, 32, &salt_id);
    psa_reset_key_attributes(&salt_attr);
    if (st != PSA_SUCCESS) return -1;

    st = psa_mac_compute(salt_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        shared, shared_len, prk, sizeof(prk), &prk_len);
    psa_destroy_key(salt_id);
    if (st != PSA_SUCCESS || prk_len != TBFT_HMAC_SIZE) {
        ESP_LOGE(TAG, "ecdh_derive: HKDF-Extract failed: %d", (int)st);
        return -1;
    }

    /* HKDF-Expand (RFC 5869): OKM = HMAC(PRK, info || 0x01)
     * For 32 bytes (< SHA-256 block size), one Expand round is sufficient.
     * The info string binds the key to its protocol role, preventing
     * cross-protocol key reuse if ECDH is ever used elsewhere. */
    static const uint8_t expand_info[] = "TinyBFT session key v1";
    uint8_t expand_in[sizeof(expand_info) + 1];
    memcpy(expand_in, expand_info, sizeof(expand_info) - 1);
    expand_in[sizeof(expand_info) - 1] = 0x01;

    psa_key_attributes_t prk_attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&prk_attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_algorithm(&prk_attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&prk_attr, PSA_KEY_USAGE_SIGN_MESSAGE);

    mbedtls_svc_key_id_t prk_id = MBEDTLS_SVC_KEY_ID_INIT;
    st = psa_import_key(&prk_attr, prk, prk_len, &prk_id);
    psa_reset_key_attributes(&prk_attr);
    if (st != PSA_SUCCESS) { memset(prk, 0, sizeof(prk)); return -1; }

    uint8_t hkdf_out[TBFT_HMAC_SIZE];
    size_t hkdf_len = 0;
    st = psa_mac_compute(prk_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
        expand_in, sizeof(expand_in), hkdf_out, sizeof(hkdf_out), &hkdf_len);
    psa_destroy_key(prk_id);
    memset(prk, 0, sizeof(prk));

    if (st != PSA_SUCCESS || hkdf_len != TBFT_HMAC_SIZE) {
        ESP_LOGE(TAG, "ecdh_derive: HKDF failed: %d", (int)st);
        return -1;
    }

    memcpy(out_key->bytes, hkdf_out, TBFT_HMAC_SIZE);
    memset(shared, 0, sizeof(shared));
    return 0;
}
