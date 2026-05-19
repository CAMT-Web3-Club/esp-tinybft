#!/usr/bin/env bash
# memcalc.sh — compute tbft_replica_t size from actual header structs + config values
set -euo pipefail

# --- config values (from sdkconfig.defaults) ---
MAX_MESSAGE_SIZE=2048
MAX_REPLY_SIZE=900
MAX_NUM_REPLICAS=7
MAX_NUM_CLIENTS=1
WINDOW_SIZE=4
CHECKPOINT_INTERVAL=2
RQUEUE_MAX=4
NDET_BUF_SIZE=256
MAX_STATE_BLOCKS=8
P_LEVELS=2
BLOCK_SIZE=1024          # default from Kconfig

# --- derived ---
DIGEST_SIZE=32
HMAC_SIZE=32
SIG_SIZE=256              # RSA-2048 / 8
AUTH_SIZE=$(( HMAC_SIZE * (MAX_NUM_REPLICAS - 1) ))   # 32*6 = 192
NUM_CKPT_SLOTS=$(( WINDOW_SIZE / CHECKPOINT_INTERVAL + 2 ))  # 4/2+2 = 4
MAX_FAULTY=$(( (MAX_NUM_REPLICAS - 1) / 3 ))            # 6/3 = 2
CERT_MAX_VALS=$(( MAX_FAULTY + 1 ))                      # 3

# --- message sizes ---
# Rep headers (packed, from tbft_message.h)
HDR_SIZE=16    # int16_t tag(2) + int16_t extra(2) + int32_t size(4) + int64_t ts(8) = 16

# Checkpoint: hdr(16) + int32_t id(4) + int64_t seqno(8) + int32_t n_digest(4) + digest(32) + int32_t epoch(4) = 68, plus AUTH after rep
CKPT_REP_SIZE=68
CKPT_MSG_SIZE=$(( CKPT_REP_SIZE + AUTH_SIZE ))  # 68 + 192 = 260

# Prepare: hdr(16) + int32_t id(4) + int64_t seqno(8) + digest(32) + int64_t timestamp(8) + int32_t epoch(4) = 72
PREPARE_REP_SIZE=72
PREPARE_MSG_SIZE=$(( PREPARE_REP_SIZE + AUTH_SIZE ))  # 72 + 192 = 264

# Commit: hdr(16) + int32_t id(4) + int64_t seqno(8) + digest(32) + int64_t timestamp(8) + int32_t epoch(4) = 72
COMMIT_REP_SIZE=72
COMMIT_MSG_SIZE=$(( COMMIT_REP_SIZE + AUTH_SIZE ))  # 72 + 192 = 264

# Pre_prepare: hdr(16) + int64_t seqno(8) + int32_t n_requests(4) + int32_t ndet_len(4) + int32_t epoch(4) = 36, + ndet + requests + AUTH
PP_REP_SIZE=36  # variable; max is MAX_MESSAGE_SIZE
PP_MAX_SIZE=$MAX_MESSAGE_SIZE

# New_key rep: hdr(16) + int32_t id(4) + int32_t n_keys(4) = 24
NK_REP_SIZE=24
# New_key slot: int32_t recipient_id(4) + ciphertext[256] = 260
NK_SLOT_SIZE=$(( 4 + SIG_SIZE ))  # 260

# View_change rep: hdr(16) + int32_t id(4) + int64_t seqno(8) + int32_t epoch(4) + digest(32) = 64
# + prepared certs... variable, max is MAX_MESSAGE_SIZE
VC_REP_SIZE=64
VC_MSG_MAX=$MAX_MESSAGE_SIZE

# VC ack rep: hdr(16) + int32_t id(4) + int32_t vc_id(4) + digest(32) = 56
VC_ACK_REP_SIZE=56

# Request rep: hdr(16) + int32_t cid(4) + int64_t rid(8) + int32_t command_size(4) + uint8_t ro(1) + pad(3) = 36
REQUEST_REP_SIZE=36
REQ_MSG_MAX=$MAX_MESSAGE_SIZE

# Reply rep: hdr(16) + int64_t rid(8) + int32_t cid(4) + int32_t reply_size(4) + int32_t status(4) + pad(4) = 40
REPLY_REP_SIZE=40
REP_MSG_MAX=$(( REPLY_REP_SIZE + MAX_REPLY_SIZE + SIG_SIZE ))  # 40 + 900 + 256 = 1196

# Status: hdr(16) + int32_t id(4) + int64_t seqno(8) + int32_t epoch(4) = 32
STATUS_REP_SIZE=32

# --- compute padded sizes (8-byte aligned for messages) ---
align8() { echo $(( ($1 + 7) & ~7 )); }

# --- 1. tbft_certificate_t (via TBFT_CERT_DECLARE macro) ---
# typedef struct {
#   tbft_bitmap_t  bmap;                    // 8
#   tbft_digest_t  val_digests[C_MAX];      // C_MAX * 32
#   uint8_t        vals[C_MAX][msg_size];   // C_MAX * msg_size
#   int            correct[C_MAX];          // C_MAX * 4
#   int            num_vals;                // 4
#   int            mym_idx;                 // 4
#   int            complete_threshold;      // 4
# } → total with natural alignment
cert_size() {
    local msg_sz=$1
    local nvals=$CERT_MAX_VALS
    local sz=0
    sz=$(( sz + 8 ))                              # bmap
    sz=$(( sz + nvals * DIGEST_SIZE ))             # val_digests
    sz=$(( sz + nvals * msg_sz ))                  # vals
    sz=$(( sz + nvals * 4 ))                       # correct
    sz=$(( sz + 4 ))                               # num_vals
    sz=$(( sz + 4 ))                               # mym_idx
    sz=$(( sz + 4 ))                               # complete_threshold
    echo $sz
}

PREPARE_CERT_SIZE=$(cert_size $PREPARE_MSG_SIZE)
COMMIT_CERT_SIZE=$(cert_size $COMMIT_MSG_SIZE)

# --- 2. tbft_prepared_cert_t ---
# pp_buf[MAX_MESSAGE_SIZE] + int pp_len(4) + padding(4) + prepare_cert + int64_t t_sent_us(8)
PREPARED_CERT_SIZE=$(( PP_MAX_SIZE + 4 + 4 + PREPARE_CERT_SIZE + 8 ))

# --- 3. tbft_agreement_slice_t ---
# prepared_cert + commit_cert
SLICE_SIZE=$(( PREPARED_CERT_SIZE + COMMIT_CERT_SIZE ))

# --- 4. tbft_agreement_region_t ---
# slices[WINDOW_SIZE] + head(8) + head_idx(4) + mask(4) + prepare_threshold(4) + commit_threshold(4)
AR_SIZE=$(( SLICE_SIZE * WINDOW_SIZE + 8 + 4 + 4 + 4 + 4 ))

# --- 5. tbft_ckpt_slot_t ---
# seqno(8) + msgs[MAX_NUM_REPLICAS][CKPT_MSG_SIZE] + msg_lens[n](n*4) + present[n](n*1+pad)
# + n_candidates(4) + cand_digests[CERT_MAX_VALS](C_MAX*32) + cand_counts[C_MAX](C_MAX*4)
# + match_count(4) + winning_digest(32)
CKPT_SLOT_SIZE=$(( 8 + MAX_NUM_REPLICAS * CKPT_MSG_SIZE + MAX_NUM_REPLICAS * 4 + MAX_NUM_REPLICAS * 1
                   + 4 + CERT_MAX_VALS * DIGEST_SIZE + CERT_MAX_VALS * 4 + 4 + DIGEST_SIZE ))
# pad present[] to 4-byte align if needed
CKPT_SLOT_PAD=$(( (CKPT_SLOT_SIZE + 3) & ~3 ))
CKPT_SLOT_SIZE=$CKPT_SLOT_PAD

# --- 6. tbft_checkpoint_region_t ---
# slots[NUM_CKPT_SLOTS] + above_window[MAX_NUM_REPLICAS] + num_replicas(4) + threshold(4)
CR_SIZE=$(( CKPT_SLOT_SIZE * NUM_CKPT_SLOTS + CKPT_SLOT_SIZE * MAX_NUM_REPLICAS + 4 + 4 ))

# --- 7. TBFT_SR_SLOT helper (the macro defines:  buf[ms]; int len; bool valid; ) ---
# Note: in the actual struct, buf is aligned. sizeof(sr_slot)
sr_slot_size() {
    local buf_sz=$1
    # uint8_t buf[buf_sz] + int len(4) + bool valid(1) + padding(3) to align to 8
    local sz=$(( buf_sz + 4 + 1 + 3 ))
    echo $sz
}

# --- 8. tbft_special_region_t ---
SR_VC_SLOT=$(sr_slot_size $VC_MSG_MAX)          # ~2056
SR_VC_ACK_SLOT=$(sr_slot_size $VC_ACK_REP_SIZE)  # ~64
SR_NEW_VIEW_SLOT=$(sr_slot_size $VC_MSG_MAX)     # ~2056  (New_view similar to View_change max)
SR_NEW_KEY_SLOT=$(sr_slot_size $MAX_MESSAGE_SIZE) # ~2056
SR_REQUEST_SLOT=$(sr_slot_size $REQ_MSG_MAX)      # ~2056
SR_REPLY_SLOT=$(sr_slot_size $REP_MSG_MAX)        # ~1204

SR_SIZE=$(( MAX_NUM_REPLICAS * SR_VC_SLOT
          + MAX_NUM_REPLICAS * MAX_NUM_REPLICAS * SR_VC_ACK_SLOT
          + SR_NEW_VIEW_SLOT
          + SR_NEW_KEY_SLOT
          + MAX_NUM_CLIENTS * SR_REQUEST_SLOT
          + MAX_NUM_CLIENTS * SR_REPLY_SLOT
          + 4 + 4 ))  # num_replicas(4), num_clients(4)

# --- 9. tbft_rqueue_entry_t ---
# buf[MAX_MESSAGE_SIZE] + len(4) + ro(1) + used(1) + pad(2)
RQUEUE_ENTRY_SIZE=$(( MAX_MESSAGE_SIZE + 4 + 1 + 1 + 2 ))

# --- 10. tbft_rqueue_t ---
# entries[RQUEUE_MAX] + head(4) + tail(4) + count(4)
RQUEUE_SIZE=$(( RQUEUE_MAX * RQUEUE_ENTRY_SIZE + 4 + 4 + 4 ))

# --- 11. tbft_state_t ---
# mem(4) + mem_size(4) + num_blocks(4) + pad(4)
# + cowb[(MAX_STATE_BLOCKS/64+1)*8]
# + ptree(~56 pointer-based)
# + block_digests[MAX_STATE_BLOCKS * 32]
# + ckpt_records[NUM_CKPT_SLOTS] (~52 each: seqno(8)+digest(32)+state_offset(4)+state_len(4)+epoch(4)=52)
# + ckpt_head(4) + ckpt_count(4)
# + in_fetch(1) + pad(7) + fetch_seqno(8)
# + fetch_queue[MAX_STATE_BLOCKS * 12] (~12 each: level(4)+idx(4)+digest_idx(4)=12)
# + fetch_queue_len(4) + n_data_pending(4) + fetch_timeout_us(8) + fetch_start_time_us(8)
# + fetch_replier(4) + pad(4)
# + fetch_received[(MAX_STATE_BLOCKS+63)/64 * 8]
# + last_stable(8)
COWB_SIZE=$(( ((MAX_STATE_BLOCKS + 63) / 64) * 8 ))  # 1*8=8 for 8 blocks
PTREE_SIZE=56  # approximate
CKPT_RECORD_SIZE=52  # seqno(8)+digest(32)+state_offset(4)+state_len(4)+epoch(4)=52
FETCH_REC_SIZE=12    # level(4)+idx(4)+digest_idx(4)=12
FETCH_QUEUE_SIZE=$(( MAX_STATE_BLOCKS * FETCH_REC_SIZE ))
FETCH_RECV_SIZE=$(( ((MAX_STATE_BLOCKS + 63) / 64) * 8 ))

STATE_SIZE=$(( 4 + 4 + 4 + 4
             + COWB_SIZE
             + PTREE_SIZE
             + MAX_STATE_BLOCKS * DIGEST_SIZE
             + NUM_CKPT_SLOTS * CKPT_RECORD_SIZE
             + 4 + 4
             + 1 + 7 + 8
             + FETCH_QUEUE_SIZE
             + 4 + 4 + 8 + 8
             + 4 + 4
             + FETCH_RECV_SIZE
             + 8 ))

# --- 12. tbft_node_t ---
# node_id(4) + max_faulty(4) + num_replicas(4) + threshold(4) + view(8) + cur_primary(4) + pad(4)
# + principals[REPLICAS+CLIENTS] (pointers: (n+c)*4)
# + num_principals(4)
# + local_principal(4) + transport(4)
# + recv_buf[MAX_MESSAGE_SIZE]
# + atimer(~16) + auth_timeout_us(8) + rid_counter(8)
PRINCIPALS_PTRS=$(( (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * 4 ))
NODE_SIZE=$(( 4 + 4 + 4 + 4 + 8 + 4 + 4
            + PRINCIPALS_PTRS
            + 4
            + 4 + 4
            + MAX_MESSAGE_SIZE
            + 16 + 8 + 8 ))

# --- 13. tbft_view_info_t ---
# This is a complex struct, approximate. Contains view-change tracking.
# Looking at the header, it's moderate: ~256 bytes
VI_SIZE=256

# --- 14. tbft_replica_t = node + scalars + rqueue*2 + ar + cr + sr + state + vi + timers + ndet_buf + out_buf + misc ---
REPLICA_SIZE=$(( NODE_SIZE
               + 8 + 4 + 8 + 8 + 8 + 8 + 8   # seqnos
               + RQUEUE_SIZE * 2              # rqueue + ro_rqueue
               + AR_SIZE
               + CR_SIZE
               + SR_SIZE
               + STATE_SIZE
               + VI_SIZE
               + 16 + 16                      # vtimer, stimer
               + 8 + 8                        # vtimer_period, stimer_period
               + 4 + 4 + 4 + 4               # callbacks
               + NDET_BUF_SIZE                # ndet_buf
               + MAX_MESSAGE_SIZE             # out_buf
               + 1 + 4 + 4 ))                 # running, yield_counter, evt_group

# --- Print breakdown ---
echo "============================================"
echo " tbft_replica_t memory breakdown"
echo "============================================"
echo ""
echo "Config:"
echo "  MAX_MESSAGE_SIZE    = $MAX_MESSAGE_SIZE"
echo "  MAX_REPLY_SIZE      = $MAX_REPLY_SIZE"
echo "  MAX_NUM_REPLICAS    = $MAX_NUM_REPLICAS (n)"
echo "  MAX_NUM_CLIENTS     = $MAX_NUM_CLIENTS"
echo "  WINDOW_SIZE         = $WINDOW_SIZE"
echo "  CHECKPOINT_INTERVAL = $CHECKPOINT_INTERVAL"
echo "  RQUEUE_MAX          = $RQUEUE_MAX"
echo "  MAX_STATE_BLOCKS    = $MAX_STATE_BLOCKS"
echo "  NDET_BUF_SIZE       = $NDET_BUF_SIZE"
echo ""
echo "Derived:"
echo "  AUTH_SIZE           = $AUTH_SIZE"
echo "  CERT_MAX_VALS       = $CERT_MAX_VALS (f+1, f=$MAX_FAULTY)"
echo "  NUM_CKPT_SLOTS      = $NUM_CKPT_SLOTS"
echo "  CKPT_MSG_SIZE       = $CKPT_MSG_SIZE"
echo "  PREPARE_MSG_SIZE    = $PREPARE_MSG_SIZE"
echo "  COMMIT_MSG_SIZE     = $COMMIT_MSG_SIZE"
echo "  NK_SLOT_SIZE        = $NK_SLOT_SIZE"
echo "  REP_MSG_MAX         = $REP_MSG_MAX"
echo ""
echo "Component sizes (bytes):"
printf "  %-30s %6d\n" "prepare_cert_t" $PREPARE_CERT_SIZE
printf "  %-30s %6d\n" "commit_cert_t" $COMMIT_CERT_SIZE
printf "  %-30s %6d\n" "prepared_cert_t" $PREPARED_CERT_SIZE
printf "  %-30s %6d\n" "agreement_slice_t" $SLICE_SIZE
printf "  %-30s %6d  (%d x slice + overhead)" "agreement_region (ar)" $AR_SIZE $WINDOW_SIZE
printf "  %-30s %6d\n" "ckpt_slot_t" $CKPT_SLOT_SIZE
printf "  %-30s %6d  (%d slots + %d above)" "checkpoint_region (cr)" $CR_SIZE $NUM_CKPT_SLOTS $MAX_NUM_REPLICAS
printf "  %-30s %6d\n" "special_region (sr)" $SR_SIZE
printf "  %-30s %6d\n" "rqueue_entry_t" $RQUEUE_ENTRY_SIZE
printf "  %-30s %6d  (x2 = %d)" "rqueue_t (single)" $RQUEUE_SIZE $((RQUEUE_SIZE*2))
printf "  %-30s %6d\n" "state_t" $STATE_SIZE
printf "  %-30s %6d\n" "node_t" $NODE_SIZE
printf "  %-30s %6d\n" "view_info_t" $VI_SIZE
echo ""
printf "  %-30s %6d\n" "tbft_replica_t TOTAL" $REPLICA_SIZE
echo ""
printf "Stack: replica_task %5d, client_task %5d\n" 8192 8192
printf "TOTAL app heap needed ~ %5d KB\n" $(( (REPLICA_SIZE + 8192) / 1024 ))
echo ""
echo "--- New_key message check ---"
NK_BODY=$(( NK_REP_SIZE + (MAX_NUM_REPLICAS - 1) * NK_SLOT_SIZE ))
NK_ALIGNED=$(align8 $NK_BODY)
NK_TOTAL=$(( NK_ALIGNED + SIG_SIZE ))
printf "  New_key body  = %d (hdr) + %d*(n-1) slot = %d\n" $NK_REP_SIZE $NK_SLOT_SIZE $NK_BODY
printf "  Aligned body  = %d\n" $NK_ALIGNED
printf "  + RSA sig     = %d\n" $NK_TOTAL
printf "  out_buf       = %d\n" $MAX_MESSAGE_SIZE
if [ $NK_TOTAL -le $MAX_MESSAGE_SIZE ]; then
    printf "  RESULT:       OK (fits with %d bytes spare)\n" $((MAX_MESSAGE_SIZE - NK_TOTAL))
else
    printf "  RESULT:       FAIL (needs %d, have %d — short by %d)\n" $NK_TOTAL $MAX_MESSAGE_SIZE $((NK_TOTAL - MAX_MESSAGE_SIZE))
fi
