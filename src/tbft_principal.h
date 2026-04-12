#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "mbedtls/pk.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Principal — represents a remote (or local) participant.
 *
 * Stores the node's address, RSA public key, and the pair of symmetric
 * HMAC session keys used for fast authenticator-based message authentication:
 *
 *   hmac_in_key  — key to verify MACs on messages *received from* this node
 *   hmac_out_key — key to generate MACs on messages *sent to* this node
 *
 * For the local principal (id == local_node_id), priv_pk holds the RSA
 * private key used for signing.
 * -------------------------------------------------------------------------- */

typedef struct {
    tbft_node_id_t  id;
    tbft_addr_t     addr;

    /* RSA public key (always present for any known principal) */
    mbedtls_pk_context  pub_pk;

    /* RSA private key (only populated for the local node) */
    mbedtls_pk_context  priv_pk;
    bool                has_priv_key;

    /* HMAC session keys */
    tbft_hmac_key_t hmac_in_key;   /* verify MACs from this principal */
    tbft_hmac_key_t hmac_out_key;  /* generate MACs for this principal */
    bool            keys_fresh;    /* true if keys have been exchanged */

    /* Monotonic timestamp of last successful MAC verification (anti-replay) */
    int64_t         last_auth_time_us;
} tbft_principal_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise a principal record.
 * @param p       Uninitialized principal
 * @param id      Node id
 * @param addr    Network address
 */
void tbft_principal_init(tbft_principal_t *p, tbft_node_id_t id,
                         const tbft_addr_t *addr);

/**
 * Load the principal's RSA public key from DER-encoded bytes.
 * @return 0 on success, negative MbedTLS error code otherwise
 */
int  tbft_principal_load_pub_key(tbft_principal_t *p,
                                 const uint8_t *der, size_t der_len);

/**
 * Load the local node's RSA private key from DER-encoded bytes.
 * @return 0 on success, negative MbedTLS error code otherwise
 */
int  tbft_principal_load_priv_key(tbft_principal_t *p,
                                  const uint8_t *der, size_t der_len);

/**
 * Release all MbedTLS contexts held by the principal.
 */
void tbft_principal_free(tbft_principal_t *p);

/* --------------------------------------------------------------------------
 * HMAC (authenticator path — hot path)
 * -------------------------------------------------------------------------- */

/**
 * Generate a MAC for an outgoing message using this principal's out-key.
 * The result is placed in @p mac.
 */
int tbft_principal_gen_mac_out(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               tbft_mac_t *mac);

/**
 * Verify a MAC on an incoming message using this principal's in-key.
 * @return true if the MAC is valid, false otherwise
 */
bool tbft_principal_verify_mac_in(const tbft_principal_t *p,
                                  const void *msg, size_t msg_len,
                                  const tbft_mac_t *mac);

/**
 * Set the in-key (key for verifying messages received from this principal).
 */
void tbft_principal_set_in_key(tbft_principal_t *p, const tbft_hmac_key_t *key);

/**
 * Set the out-key (key for authenticating messages sent to this principal).
 */
void tbft_principal_set_out_key(tbft_principal_t *p, const tbft_hmac_key_t *key);

/* --------------------------------------------------------------------------
 * RSA signature path (slow path — used for Request, View_change, New_view)
 * -------------------------------------------------------------------------- */

/**
 * Sign @p msg with the local RSA private key.
 * @param p         Local principal (must have priv key loaded)
 * @param msg       Data to sign
 * @param msg_len   Length of data
 * @param sig       Output signature buffer (must be TBFT_SIG_SIZE bytes)
 * @return 0 on success, negative error code otherwise
 */
int tbft_principal_sign(tbft_principal_t *p,
                        const void *msg, size_t msg_len,
                        tbft_sig_t *sig);

/**
 * Verify an RSA signature against this principal's public key.
 * @return true if valid, false otherwise
 */
bool tbft_principal_verify_sig(const tbft_principal_t *p,
                               const void *msg, size_t msg_len,
                               const tbft_sig_t *sig);

/* --------------------------------------------------------------------------
 * Session key rotation (New_key message)
 * -------------------------------------------------------------------------- */

/**
 * Generate fresh random session keys for this principal and RSA-encrypt
 * the new out-key under the principal's public key.
 *
 * @param p            Target principal
 * @param new_out_key  The newly generated out-key (plaintext, caller stores)
 * @param enc_buf      Buffer to receive encrypted key (RSA ciphertext)
 * @param enc_buf_len  Size of enc_buf (must be >= TBFT_SIG_SIZE)
 * @param out_enc_len  Receives actual ciphertext length
 * @return 0 on success
 */
int tbft_principal_encrypt_new_key(tbft_principal_t *p,
                                   const tbft_hmac_key_t *new_out_key,
                                   uint8_t *enc_buf, size_t enc_buf_len,
                                   size_t *out_enc_len);

/**
 * Decrypt an RSA-encrypted session key received via New_key message.
 *
 * @param p            Local principal (needs private key)
 * @param enc_buf      Encrypted key bytes
 * @param enc_len      Encrypted key length
 * @param new_in_key   Receives the decrypted key
 * @return 0 on success
 */
int tbft_principal_decrypt_new_key(tbft_principal_t *p,
                                   const uint8_t *enc_buf, size_t enc_len,
                                   tbft_hmac_key_t *new_in_key);
