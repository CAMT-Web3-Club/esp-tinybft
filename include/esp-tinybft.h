#pragma once

/**
 * @file esp-tinybft.h
 * @brief TinyBFT public API for ESP-IDF (libbyz-compatible interface).
 *
 * TinyBFT implements PBFT (Practical Byzantine Fault Tolerance) with static
 * memory regions optimised for embedded systems.  This header exposes the
 * client and replica APIs mirroring the original libbyz interface.
 *
 * ## Transport Layer Selection
 *
 * The component supports two interchangeable transport backends, selected at
 * compile time via `CONFIG_TBFT_TRANSPORT_TYPE` in `idf.py menuconfig`
 * (under Component config → TinyBFT Configuration → Transport Type):
 *
 * | Backend    | Config Value           | Protocol     | Max Payload    | Use Case                  |
 * |------------|------------------------|--------------|----------------|---------------------------|
 * | **UDP**    | `TBFT_TRANSPORT_UDP`   | lwIP sockets | Unbounded      | Ethernet/Wi-Fi STA + AP   |
 * | **ESP-NOW**| `TBFT_TRANSPORT_ESPNOW`| esp_now API  | 1470 bytes     | Direct device-to-device   |
 *
 * ### UDP Transport
 * Uses standard lwIP UDP sockets. Supports multicast for broadcast sends.
 * Requires the host application to initialise the network stack (WiFi or
 * Ethernet) before calling `Byz_init_replica()`. The config file uses
 * IP addresses and ports to identify peers.
 *
 * ### ESP-NOW Transport
 * Uses the ESP-NOW API for direct device-to-device communication without
 * requiring an access point. Messages exceeding 1470 bytes are automatically
 * fragmented and reassembled. The config file uses MAC addresses to identify
 * peers. The host application must:
 *   1. Initialise WiFi (`esp_wifi_init`, `esp_wifi_set_mode`, `esp_wifi_start`)
 *   2. Call `esp_now_init()` before `Byz_init_replica()`
 *   3. Ensure the device is in STA or AP+STA mode (required by ESP-NOW)
 *
 * Switching transports requires **no code changes** — only the menuconfig
 * setting and the config file format differ. Both backends handle the same
 * message formats, HMAC authentication, and RSA signatures.
 *
 * Typical client usage:
 * @code
 *   Byz_req req;
 *   Byz_rep rep;
 *   // local_id 4 = first client slot in a 4-replica config (3f+1 with f=1)
 *   Byz_init_client("config.txt", "priv.txt", 4, 0);
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
 *   // local_id identifies which replica slot (0..num_nodes-1) this board is
 *   Byz_init_replica("config.txt", "priv.txt", 0,
 *                    app_state, sizeof(app_state),
 *                    exec_cb, NULL, 0, NULL, 0);
 *   Byz_replica_run();  // blocks — run in a FreeRTOS task
 * @endcode
 *
 * @note Thread safety: Byz_init_replica() is NOT thread-safe with respect
 * to Byz_replica_run(). The caller must ensure Byz_replica_run() has exited
 * (the FreeRTOS task has terminated) before calling Byz_init_replica() again.
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
 * @param local_id      Node slot this client occupies in the config
 *                      (must be in [num_replicas, num_nodes-1]);
 *                      pass -1 to auto-select the first client slot (3f+1)
 * @param port          UDP port to bind (0 = OS-assigned)
 * @return 0 on success, -1 on error
 */
int Byz_init_client(const char *config_file, const char *priv_config,
                    int local_id, uint16_t port);

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

/**
 * Send a request and wait for a reply, with bounded retry and exponential
 * backoff.  Used by clients that need to survive a request being abandoned
 * by the cluster (e.g., a primary withholding a pre-prepare so the fill
 * mechanism eventually times out).
 *
 * Each attempt calls `Byz_invoke()`, which has its own per-attempt timeout
 * of `TBFT_CLIENT_REPLY_TIMEOUT_MS` (default 10s).  On timeout, the call
 * sleeps for `delay_ms` (doubling each attempt, capped at 10s) and retries
 * until `max_attempts` is reached.
 *
 * @param req         Populated request buffer (must remain valid for the
 *                    duration of the call).  Each attempt resubmits the same
 *                    request; the underlying transport increments rid, so
 *                    retries are treated as fresh consensus operations.
 * @param rep         Receives the agreed reply on success.
 * @param read_only   True if the request is read-only.
 * @param max_attempts Maximum number of attempts (must be >= 1).  Use a
 *                    small value (e.g., 3) for non-idempotent requests and
 *                    a larger value (e.g., 8) for idempotent ones.
 * @return Number of attempts used on success (1..max_attempts), or -1 if
 *         all attempts timed out / errored.
 */
int Byz_invoke_with_retry(Byz_req *req, Byz_rep *rep, bool read_only,
                          int max_attempts);

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
 * @param local_id      Replica slot this board occupies (0..3f). Must match
 *                      the node entry in @p config_file whose @p priv_config
 *                      corresponds to; otherwise peers will reject this
 *                      replica's authenticators.
 *                      Pass -1 to auto-detect from the local WiFi MAC
 *                      (ESP-NOW only; WiFi must be started first).
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
                     int local_id,
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

/**
 * Auto-detect the local node ID from the cluster config file.
 *
 * For ESP-NOW transport, reads the WiFi STA MAC and matches it against
 * the MAC addresses in the config. For UDP transport, reads the local
 * STA IP and matches it against the IP addresses in the config.
 *
 * WiFi (ESP-NOW) or network (UDP) must be initialised and started
 * before calling this function.
 *
 * @param config_file   Path to the cluster configuration file
 * @return node index (0-based) if found, -1 if not detected
 */
int Byz_detect_local_id(const char *config_file);

/** Reset all performance counters. */
void Byz_reset_stats(void);

/** Print current performance statistics to the ESP-IDF log. */
void Byz_print_stats(void);

#ifdef __cplusplus
}
#endif
