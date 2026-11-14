/**
 * @file stub_principal.c
 * @brief Host-simulator replacement for src/tbft_principal.c (no real crypto).
 *
 * Implements the API of src/tbft_principal.h so that the key-exchange state
 * (hmac_in/out keys, keys_fresh, psa key ids, New_key ciphertext caches)
 * evolves exactly as on hardware, without RSA:
 *
 *  - Keys: a key file is any byte string; its SHA-256 is the key's
 *    "fingerprint".  The orchestrator writes the same bytes to pubN.key and
 *    privN.key, so the pair check in tbft_libbyz.c still works.
 *  - Signatures: 256 deterministic bytes derived from the signer's
 *    fingerprint and the signed bytes.  Verification recomputes them with
 *    the claimed signer's public fingerprint (SIM_AUTH=strict, default), or
 *    always succeeds (SIM_AUTH=lax).
 *  - MACs: HMAC-SHA256 under the principal's session key, exactly what the
 *    real code computes.  Verification uses the in-key (strict) or always
 *    succeeds (lax).  Strict mode reproduces the hardware's rejection of a
 *    message MACed with a key the receiver has not installed yet (e.g.
 *    right after a peer reboots) and of any message modified after it was
 *    MACed/signed.
 *  - New_key: "encryption" copies the session key into a 256-byte block
 *    together with the recipient's fingerprint and random padding (so two
 *    encryptions of the same key differ, like RSA-OAEP); "decryption"
 *    checks the recipient (strict) and copies the key back.
 *  - Optional CPU-time model: SIM_SIGN_MS, SIM_VERIFY_MS, SIM_ENC_MS,
 *    SIM_DEC_MS busy-wait that many ms per operation (default 0).
 */

#include "tbft_principal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sim_rt.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static constexpr char TAG[] = "tbft_principal";

static bool s_lax;
static int  s_sign_ms, s_verify_ms, s_enc_ms, s_dec_ms;
static atomic_uint s_next_key_id = 1;

__attribute__((constructor)) static void stub_principal_init(void)
{
    const char *mode = getenv("SIM_AUTH");
    s_lax = (mode && strcmp(mode, "lax") == 0);
    s_sign_ms   = sim_rt_env_int("SIM_SIGN_MS", 0);
    s_verify_ms = sim_rt_env_int("SIM_VERIFY_MS", 0);
    s_enc_ms    = sim_rt_env_int("SIM_ENC_MS", 0);
    s_dec_ms    = sim_rt_env_int("SIM_DEC_MS", 0);
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_principal_ensure_psa(void)
{
    (void)psa_crypto_init();
}

void tbft_principal_init(tbft_principal_t *p, tbft_node_id_t id,
                         const tbft_addr_t *addr)
{
    memset(p, 0, sizeof(*p));
    p->id   = id;
    p->addr = *addr;
    mbedtls_pk_init(&p->pub_pk);
    mbedtls_pk_init(&p->priv_pk);
}

int tbft_principal_load_pub_key(tbft_principal_t *p,
                                const uint8_t *der, size_t der_len)
{
    int ret = sim_pk_parse(&p->pub_pk, der, der_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse_public_key failed: -0x%04x", (unsigned)(-ret));
    }
    return ret;
}

int tbft_principal_load_priv_key(tbft_principal_t *p,
                                 const uint8_t *der, size_t der_len)
{
    int ret = sim_pk_parse(&p->priv_pk, der, der_len);
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
    mbedtls_pk_free(&p->pub_pk);
    mbedtls_pk_free(&p->priv_pk);
    memset(p, 0, sizeof(*p));
}

/* --------------------------------------------------------------------------
 * HMAC
 * -------------------------------------------------------------------------- */

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac)
{
    /* Real code: PSA HMAC with the cached out-key handle, or a transient
     * import of hmac_out_key; both MAC under hmac_out_key. */
    sim_hmac_sha256(p->hmac_out_key.bytes, sizeof(p->hmac_out_key.bytes),
                    msg, msg_len, mac->bytes);
    return 0;
}

bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac)
{
    if (s_lax) return true;
    tbft_mac_t expect;
    sim_hmac_sha256(p->hmac_in_key.bytes, sizeof(p->hmac_in_key.bytes),
                    msg, msg_len, expect.bytes);
    return ct_equal(expect.bytes, mac->bytes, TBFT_HMAC_SIZE);
}

bool tbft_principal_verify_mac_in_with_replay_check(tbft_principal_t *p,
                                                     const void *msg, size_t msg_len,
                                                     const tbft_mac_t *mac,
                                                     [[maybe_unused]] int64_t msg_time_us)
{
    bool mac_ok = tbft_principal_verify_mac_in(p, msg, msg_len, mac);
    if (!mac_ok) {
        ESP_LOGW(TAG, "verify_mac_in: FAILED for id=%d (in_key set=%d, msg_len=%zu, mac[0..3]=%02x%02x%02x%02x)",
                 (int)p->id, p->psa_hmac_in_id != 0,
                 msg_len, mac->bytes[0], mac->bytes[1], mac->bytes[2], mac->bytes[3]);
        return false;
    }
    /* Timestamp check intentionally disabled, as in the real implementation. */
    return true;
}

void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    p->hmac_in_key = *key;
    p->psa_hmac_in_id = atomic_fetch_add(&s_next_key_id, 1);
    /* Real code: keys_fresh = (import ok && self-test ok); both always
     * succeed for a 32-byte HMAC key. */
    p->keys_fresh = true;
    ESP_LOGD(TAG, "set_in_key: id=%d key[0..3]=%02x%02x%02x%02x psa_id=%u",
             (int)p->id, key->bytes[0], key->bytes[1], key->bytes[2], key->bytes[3],
             (unsigned)p->psa_hmac_in_id);
    p->last_auth_time_us = 0;
}

void tbft_principal_set_out_key(tbft_principal_t *p, const tbft_hmac_key_t *key)
{
    p->hmac_out_key = *key;
    p->psa_hmac_out_id = atomic_fetch_add(&s_next_key_id, 1);
    ESP_LOGD(TAG, "set_out_key: for id=%d, key[0..3]=%02x%02x%02x%02x",
             (int)p->id, key->bytes[0], key->bytes[1], key->bytes[2], key->bytes[3]);
}

/* --------------------------------------------------------------------------
 * "RSA" signatures
 * -------------------------------------------------------------------------- */

static void make_sig(const unsigned char fp[32], const void *msg, size_t len,
                     tbft_sig_t *sig)
{
    static const char dom[] = "tinybft-sim-signature";
    uint8_t h[32];
    sim_sha256_ctx c;
    sim_sha256_init(&c);
    sim_sha256_update(&c, dom, sizeof(dom));
    sim_sha256_update(&c, fp, 32);
    sim_sha256_update(&c, msg, len);
    sim_sha256_final(&c, h);
    for (int i = 0; i < TBFT_SIG_SIZE / 32; i++) {
        uint8_t blk[33];
        memcpy(blk, h, 32);
        blk[32] = (uint8_t)i;
        sim_sha256(blk, sizeof(blk), sig->bytes + 32 * i);
    }
}

int tbft_principal_sign(tbft_principal_t *p,
                        const void *msg, size_t msg_len,
                        tbft_sig_t *sig)
{
    if (!p->has_priv_key) {
        ESP_LOGE(TAG, "sign: no private key loaded for id=%d", p->id);
        return -1;
    }
    sim_rt_busy_ms(s_sign_ms);
    make_sig(p->priv_pk.fingerprint, msg, msg_len, sig);
    return 0;
}

bool tbft_principal_verify_sig(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               const tbft_sig_t *sig)
{
    sim_rt_busy_ms(s_verify_ms);
    if (s_lax) return true;
    if (!p->pub_pk.loaded) return false;
    tbft_sig_t expect;
    make_sig(p->pub_pk.fingerprint, msg, msg_len, &expect);
    return ct_equal(expect.bytes, sig->bytes, TBFT_SIG_SIZE);
}

/* --------------------------------------------------------------------------
 * New_key "encryption"
 * Block layout (TBFT_SIG_SIZE bytes):
 *   [0..7]   magic "SIMWRAP1"
 *   [8..39]  recipient public-key fingerprint
 *   [40..71] the session key
 *   [72..]   random padding (fresh per encryption, like OAEP)
 * -------------------------------------------------------------------------- */

static const uint8_t WRAP_MAGIC[8] = { 'S', 'I', 'M', 'W', 'R', 'A', 'P', '1' };

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
    if (!p->pub_pk.loaded) {
        ESP_LOGE(TAG, "pk_import_into_psa (enc) failed: -0x%04x", 0x3E80u);
        return -1;
    }
    sim_rt_busy_ms(s_enc_ms);
    memcpy(enc_buf, WRAP_MAGIC, 8);
    memcpy(enc_buf + 8, p->pub_pk.fingerprint, 32);
    memcpy(enc_buf + 40, new_out_key->bytes, sizeof(new_out_key->bytes));
    esp_fill_random(enc_buf + 72, TBFT_SIG_SIZE - 72);
    *out_enc_len = TBFT_SIG_SIZE;
    return 0;
}

int tbft_principal_decrypt_new_key(tbft_principal_t *p,
                                   const uint8_t *enc_buf, size_t enc_len,
                                   tbft_hmac_key_t *new_in_key)
{
    if (!p || !enc_buf || !new_in_key) return -1;
    if (!p->has_priv_key) {
        ESP_LOGE(TAG, "decrypt_new_key: no private key loaded for id=%d", p->id);
        return -1;
    }
    sim_rt_busy_ms(s_dec_ms);
    if (enc_len != TBFT_SIG_SIZE) {
        ESP_LOGE(TAG, "psa_asymmetric_decrypt failed: %d", (int)PSA_ERROR_INVALID_ARGUMENT);
        return (int)PSA_ERROR_INVALID_ARGUMENT;
    }
    if (!s_lax && (memcmp(enc_buf, WRAP_MAGIC, 8) != 0 ||
                   memcmp(enc_buf + 8, p->priv_pk.fingerprint, 32) != 0)) {
        /* Wrong recipient / garbage: RSA-OAEP decryption fails. */
        ESP_LOGE(TAG, "psa_asymmetric_decrypt failed: %d", (int)PSA_ERROR_INVALID_SIGNATURE);
        return (int)PSA_ERROR_INVALID_SIGNATURE;
    }
    memcpy(new_in_key->bytes, enc_buf + 40, sizeof(new_in_key->bytes));
    return 0;
}
