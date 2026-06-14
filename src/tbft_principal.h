#pragma once

#include "tbft_types.h"
#include "tbft_config.h"
#include "tbft_message.h"
#include "psa/crypto.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Principal — ECDSA P-256 + ECDH, pure PSA Crypto.
 *
 * All RSA removed.  Key pairs are SECP256R1 (P-256).
 *   - ECDSA PKCS#1-v1.5-style (deterministic) for signing/verification
 *   - ECDH for session key agreement (New_key exchange)
 *
 * Zero mbedtls/pk dependency — raw bytes imported via psa_import_key().
 * -------------------------------------------------------------------------- */

#define TBFT_ECDSA_KEY_BITS   256
#define TBFT_ECDSA_PRIV_SIZE  32     /* P-256 raw private key */
#define TBFT_ECDSA_PUB_SIZE   65     /* 0x04 || X(32) || Y(32) uncompressed */

typedef struct {
    tbft_node_id_t  id;
    tbft_addr_t     addr;

    /* ECDSA public key handle (PSA) — always present for any known principal */
    mbedtls_svc_key_id_t pub_verify_id;

    /* ECDSA private key handles (PSA — only populated for the local node).
     * Two handles: one for signing, one for ECDH key agreement. */
    mbedtls_svc_key_id_t priv_sign_id;
    mbedtls_svc_key_id_t priv_ecdh_id;

    /* HMAC session keys — two-key window for rotation grace period */
    tbft_hmac_key_t hmac_in_key;
    tbft_hmac_key_t hmac_out_key;
    bool            keys_fresh;

    /* Previous in_key — retained for one rotation cycle so that
     * in-flight protocol messages signed with the old key before
     * handle_new_key processed the new key don't fail MAC verification.
     * Verified by verify_mac_in_with_replay_check as a fallback. */
    tbft_hmac_key_t       hmac_in_key_prev;
    mbedtls_svc_key_id_t  psa_hmac_in_id_prev;

    /* B-FIX: Previous out_key — retained for one rotation cycle so that
     * the SENDER (primary) keeps signing with the OLD key during the
     * grace window between broadcast-New_key and replica-acknowledge.
     * Without this, the primary's out_key switches immediately in
     * tbft_principal_set_out_key, but replicas haven't received the
     * New_key broadcast yet, so they verify with the OLD in_key and
     * the MAC fails (no fallback available on the receiver side because
     * hmac_in_key_prev is OLD-OLD, not OLD).
     *
     * Symmetric to hmac_in_key_prev on the receiver side: the receiver
     * has a two-key window for OLD/NEW in_key; the sender has the same
     * two-key window for OLD/NEW out_key. */
    tbft_hmac_key_t       hmac_out_key_old;
    mbedtls_svc_key_id_t  psa_hmac_out_id_old;
    int64_t               out_key_old_expires_us;  /* 0 = no grace active */

    /* Persistent PSA key handles for HMAC */
    mbedtls_svc_key_id_t psa_hmac_in_id;
    mbedtls_svc_key_id_t psa_hmac_out_id;

    int64_t         last_auth_time_us;

    /* Cached public key bytes for ECDH (65 bytes uncompressed).
     * Needed because psa_export_public_key on SECP256R1 produces 65 bytes. */
    uint8_t         pub_key_bytes[TBFT_ECDSA_PUB_SIZE];
} tbft_principal_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_principal_init(tbft_principal_t *p, tbft_node_id_t id,
                         const tbft_addr_t *addr);

int  tbft_principal_load_pub_key(tbft_principal_t *p,
                                 const uint8_t *pub_bytes, size_t pub_len);

int  tbft_principal_load_priv_key(tbft_principal_t *p,
                                  const uint8_t *priv_bytes, size_t priv_len);

void tbft_principal_free(tbft_principal_t *p);

void tbft_principal_ensure_psa(void);

/* --------------------------------------------------------------------------
 * HMAC (authenticator path — hot path, unchanged)
 * -------------------------------------------------------------------------- */

int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac);

bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac);

bool tbft_principal_verify_mac_in_with_replay_check(tbft_principal_t *p,
                                                    const void *msg, size_t msg_len,
                                                    const tbft_mac_t *mac,
                                                    int64_t msg_time_us);

static inline void tbft_principal_update_auth_time(tbft_principal_t *p, int64_t time_us)
{
    p->last_auth_time_us = time_us;
}

void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key);
void tbft_principal_set_out_key(tbft_principal_t *p, const tbft_hmac_key_t *key);

/* B-FIX: Commit the pending old out_key (frees the backup PSA handle).
 * Called by the replica's rotate_key_tick after the grace period expires.
 * Safe to call when no grace is active (no-op). */
void tbft_principal_commit_out_key(tbft_principal_t *p);

/* B-FIX: Returns true if this principal is currently in the out_key
 * grace period (signing with the old key while peers catch up). */
bool tbft_principal_in_out_key_grace(const tbft_principal_t *p);

/* --------------------------------------------------------------------------
 * ECDSA signature path
 * -------------------------------------------------------------------------- */

int tbft_principal_sign(tbft_principal_t *p,
                        const void *msg, size_t msg_len,
                        tbft_sig_t *sig);

bool tbft_principal_verify_sig(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               const tbft_sig_t *sig);

/* --------------------------------------------------------------------------
 * Session key derivation (ECDH — replaces RSA encrypt/decrypt)
 * -------------------------------------------------------------------------- */

/**
 * Derive a fresh HMAC session key for a peer using ECDH.
 * Computes: ECDH(my_priv, peer_pub) → shared_secret → HKDF(salt) → key
 * The salt (32 random bytes) is sent in the New_key message.
 *
 * @param p         Local principal (with private key loaded)
 * @param peer      Peer principal (with public key loaded)
 * @param salt      32-byte random salt (from New_key message)
 * @param out_key   Output HMAC key
 * @return 0 on success
 */
int tbft_principal_ecdh_derive_key(const tbft_principal_t *p,
                                   const tbft_principal_t *peer,
                                   const uint8_t salt[32],
                                   tbft_hmac_key_t *out_key);
