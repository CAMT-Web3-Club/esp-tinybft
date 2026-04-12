#pragma once

/**
 * @file esp-tinybft.h
 * @brief TinyBFT public API for ESP-IDF (libbyz-compatible interface).
 *
 * TinyBFT implements PBFT (Practical Byzantine Fault Tolerance) with static
 * memory regions optimised for embedded systems.  This header exposes the
 * client and replica APIs mirroring the original libbyz interface.
 *
 * Typical client usage:
 * @code
 *   Byz_req req;
 *   Byz_rep rep;
 *   Byz_init_client("config.txt", "priv.txt", 0);
 *   Byz_alloc_request(&req, 64);
 *   memcpy(req.contents, my_cmd, 64);
 *   req.size = 64;
 *   Byz_invoke(&req, &rep, false);
 *   // process rep.contents[0..rep.size)
 *   Byz_free_reply(&rep);
 *   Byz_free_request(&req);
 * @endcode
 *
 * Typical replica usage:
 * @code
 *   static uint8_t app_state[4096];
 *   Byz_init_replica("config.txt", "priv.txt",
 *                    app_state, sizeof(app_state),
 *                    exec_cb, NULL, 0, NULL, 0);
 *   Byz_replica_run();  // blocks — run in a FreeRTOS task
 * @endcode
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Common buffer types
 * -------------------------------------------------------------------------- */

/** Request buffer (passed to Byz_alloc_request / Byz_send_request) */
typedef struct {
    char *contents; /**< Command payload (allocated by Byz_alloc_request) */
    int   size;     /**< Size of the payload in bytes */
} Byz_req;

/** Reply buffer (filled by Byz_recv_reply) */
typedef struct {
    char *contents; /**< Reply payload */
    int   size;     /**< Size of reply payload in bytes */
} Byz_rep;

/** Generic byte buffer passed as non-deterministic choices */
typedef struct {
    char *contents;
    int   size;
} Byz_buffer;

/* --------------------------------------------------------------------------
 * Callback signatures (Replica API)
 * -------------------------------------------------------------------------- */

/**
 * Execute callback: called by the library when a request is committed.
 *
 * @param in       Committed request payload
 * @param insize   Length of @p in
 * @param out      Buffer to write the reply into
 * @param outsize  [in/out] max bytes available / bytes written
 * @param ndet     Non-deterministic choices buffer
 * @param cid      Client id
 * @param ro       True if this is a read-only request
 * @return 0 on success, -1 to reject (no reply sent)
 */
typedef int (*Byz_exec_cb)(Byz_req *in, Byz_rep *out, Byz_buffer *ndet,
                           int cid, bool ro);

/**
 * Compute non-deterministic choices callback.
 *
 * @param seqno    Sequence number being ordered
 * @param ndet     Buffer to fill with choices
 * @param max_len  Maximum bytes available in ndet
 */
typedef void (*Byz_comp_ndet_cb)(int64_t seqno, Byz_buffer *ndet, int max_len);

/**
 * Reply notification callback: called when the library is about to send
 * a reply to client @p cid.
 */
typedef void (*Byz_recv_reply_cb)(Byz_rep *reply, int cid);

/* --------------------------------------------------------------------------
 * Client API
 * -------------------------------------------------------------------------- */

/**
 * Initialise the BFT client.
 *
 * @param config_file   Path to the cluster configuration file
 * @param priv_config   Path to the private key / identity file
 * @param port          UDP port to bind (0 = OS-assigned)
 * @return 0 on success, -1 on error
 */
int Byz_init_client(const char *config_file, const char *priv_config,
                    uint16_t port);

/**
 * Allocate a request buffer of @p size bytes.
 * The caller fills req->contents and sets req->size before calling
 * Byz_send_request().
 *
 * @return 0 on success, -1 on allocation failure
 */
int Byz_alloc_request(Byz_req *req, int size);

/**
 * Send the request to the primary replica.
 *
 * @param req       Populated request buffer
 * @param read_only True if the request is read-only (no state modification)
 * @return 0 on success, -1 on error
 */
int Byz_send_request(Byz_req *req, bool read_only);

/**
 * Wait for f+1 matching replies.
 *
 * @param rep  Receives the agreed reply
 * @return 0 on success, -1 on timeout or mismatch
 */
int Byz_recv_reply(Byz_rep *rep);

/**
 * Convenience: send request and receive reply in one call.
 *
 * @return 0 on success, -1 on error
 */
int Byz_invoke(Byz_req *req, Byz_rep *rep, bool read_only);

/** Free a request buffer allocated by Byz_alloc_request(). */
void Byz_free_request(Byz_req *req);

/** Free a reply buffer filled by Byz_recv_reply(). */
void Byz_free_reply(Byz_rep *rep);

/** Reset all pending requests and client state. */
void Byz_reset_client(void);

/* --------------------------------------------------------------------------
 * Replica API
 * -------------------------------------------------------------------------- */

/**
 * Initialise the BFT replica.
 *
 * @param config_file   Cluster configuration file path
 * @param priv_config   Private key / identity file path
 * @param mem           Application state buffer (must remain valid)
 * @param mem_size      State buffer size (multiple of BLOCK_SIZE)
 * @param exec_cb       Execution callback (must not be NULL)
 * @param comp_ndet_cb  Non-deterministic choices callback (may be NULL)
 * @param ndet_max_len  Max bytes for non-deterministic choices
 * @param recv_reply_cb Reply notification callback (may be NULL)
 * @param port          UDP port (0 = from config file)
 * @return 0 on success, -1 on error
 */
int Byz_init_replica(const char *config_file, const char *priv_config,
                     void *mem, size_t mem_size,
                     Byz_exec_cb exec_cb,
                     Byz_comp_ndet_cb comp_ndet_cb, int ndet_max_len,
                     Byz_recv_reply_cb recv_reply_cb,
                     uint16_t port);

/**
 * Notify the library that the region [mem, mem+size) of application state
 * is about to be modified.  Must be called before any write to state.
 *
 * @param mem  Pointer within the state buffer
 * @param size Number of bytes to be modified
 */
void Byz_modify(void *mem, int size);

/**
 * Notify the library that a single state block containing @p mem is about
 * to be modified.
 */
void Byz_modify1(void *mem);

/**
 * Run the replica event loop.  This function blocks indefinitely; call it
 * from a dedicated FreeRTOS task.
 */
void Byz_replica_run(void);

/** Reset all performance counters. */
void Byz_reset_stats(void);

/** Print current performance statistics to the ESP-IDF log. */
void Byz_print_stats(void);

#ifdef __cplusplus
}
#endif
