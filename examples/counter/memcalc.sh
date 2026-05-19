#!/usr/bin/env bash
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
BLOCK_SIZE=1024

# --- derived ---
DIGEST_SIZE=32
HMAC_SIZE=32
SIG_SIZE=256
AUTH_SIZE=$(( HMAC_SIZE * (MAX_NUM_REPLICAS - 1) ))
NUM_CKPT_SLOTS=$(( WINDOW_SIZE / CHECKPOINT_INTERVAL + 2 ))
MAX_FAULTY=$(( (MAX_NUM_REPLICAS - 1) / 3 ))
CERT_MAX_VALS=$(( MAX_FAULTY + 1 ))

# --- message sizes ---
HDR_SIZE=16
CKPT_REP_SIZE=68
CKPT_MSG_SIZE=$(( CKPT_REP_SIZE + AUTH_SIZE ))
PREPARE_REP_SIZE=72
PREPARE_MSG_SIZE=$(( PREPARE_REP_SIZE + AUTH_SIZE ))
COMMIT_REP_SIZE=72
COMMIT_MSG_SIZE=$(( COMMIT_REP_SIZE + AUTH_SIZE ))
PP_MAX_SIZE=$MAX_MESSAGE_SIZE
NK_REP_SIZE=24
NK_SLOT_SIZE=$(( 4 + SIG_SIZE ))
VC_REP_SIZE=64
VC_MSG_MAX=$MAX_MESSAGE_SIZE
VC_ACK_REP_SIZE=56
REQUEST_REP_SIZE=36
REQ_MSG_MAX=$MAX_MESSAGE_SIZE
REPLY_REP_SIZE=40
REP_MSG_MAX=$(( REPLY_REP_SIZE + MAX_REPLY_SIZE + SIG_SIZE ))

align8() { echo $(( ($1 + 7) & ~7 )); }

# --- certificate size helper ---
cert_size() {
    local msg_sz=$1
    local nvals=$CERT_MAX_VALS
    local sz=0
    sz=$(( sz + 8 ))
    sz=$(( sz + nvals * DIGEST_SIZE ))
    sz=$(( sz + nvals * msg_sz ))
    sz=$(( sz + nvals * 4 ))
    sz=$(( sz + 4 ))
    sz=$(( sz + 4 ))
    sz=$(( sz + 4 ))
    echo $sz
}

# --- slot size helper (buf + len + valid + pad to 8) ---
sr_slot_size() {
    local buf_sz=$1
    echo $(( buf_sz + 8 ))
}

# --- compute all sizes ---
PREPARE_CERT_SIZE=$(cert_size $PREPARE_MSG_SIZE)
COMMIT_CERT_SIZE=$(cert_size $COMMIT_MSG_SIZE)
PREPARED_CERT_SIZE=$(( PP_MAX_SIZE + 4 + 4 + PREPARE_CERT_SIZE + 8 ))
SLICE_SIZE=$(( PREPARED_CERT_SIZE + COMMIT_CERT_SIZE ))

AR_OVERHEAD=24  # head(8)+head_idx(4)+mask(4)+prepare_thr(4)+commit_thr(4)
AR_SIZE=$(( SLICE_SIZE * WINDOW_SIZE + AR_OVERHEAD ))

CKPT_SLOT_SIZE=$(( 8
    + MAX_NUM_REPLICAS * CKPT_MSG_SIZE
    + MAX_NUM_REPLICAS * 4
    + MAX_NUM_REPLICAS
    + 4 + CERT_MAX_VALS * DIGEST_SIZE + CERT_MAX_VALS * 4 + 4 + DIGEST_SIZE ))
CKPT_SLOT_SIZE=$(( (CKPT_SLOT_SIZE + 7) & ~7 ))

CR_SIZE=$(( CKPT_SLOT_SIZE * NUM_CKPT_SLOTS + CKPT_SLOT_SIZE * MAX_NUM_REPLICAS + 8 ))

SR_VC_SLOT=$(sr_slot_size $VC_MSG_MAX)
SR_VC_ACK_SLOT=$(sr_slot_size $VC_ACK_REP_SIZE)
SR_NV_SLOT=$(sr_slot_size $VC_MSG_MAX)
SR_NK_SLOT=$(sr_slot_size $MAX_MESSAGE_SIZE)
SR_REQ_SLOT=$(sr_slot_size $REQ_MSG_MAX)
SR_REP_SLOT=$(sr_slot_size $REP_MSG_MAX)

SR_VC_TOTAL=$(( MAX_NUM_REPLICAS * SR_VC_SLOT ))
SR_VC_ACK_TOTAL=$(( MAX_NUM_REPLICAS * MAX_NUM_REPLICAS * SR_VC_ACK_SLOT ))
SR_OVERHEAD=8  # num_replicas(4)+num_clients(4)

SR_SIZE=$(( SR_VC_TOTAL + SR_VC_ACK_TOTAL + SR_NV_SLOT + SR_NK_SLOT
          + MAX_NUM_CLIENTS * SR_REQ_SLOT + MAX_NUM_CLIENTS * SR_REP_SLOT
          + SR_OVERHEAD ))

RQUEUE_ENTRY_SIZE=$(( MAX_MESSAGE_SIZE + 8 ))  # buf(2048)+len(4)+ro(1)+used(1)+pad(2)
RQUEUE_SIZE_ONE=$(( RQUEUE_MAX * RQUEUE_ENTRY_SIZE + 12 ))  # entries + head(4)+tail(4)+count(4)
RQUEUE_BOTH=$(( RQUEUE_SIZE_ONE * 2 ))

COWB_SIZE=$(( ((MAX_STATE_BLOCKS + 63) / 64) * 8 ))
PTREE_EST=56
CKPT_RECORD_SIZE=52
FETCH_QUEUE=$(( MAX_STATE_BLOCKS * 12 ))
FETCH_RECV=$(( ((MAX_STATE_BLOCKS + 63) / 64) * 8 ))

STATE_SIZE=$(( 16 + COWB_SIZE + PTREE_EST
    + MAX_STATE_BLOCKS * DIGEST_SIZE
    + NUM_CKPT_SLOTS * CKPT_RECORD_SIZE
    + 8 + 8 + 8
    + FETCH_QUEUE + 4 + 4 + 8 + 8 + 4 + 4
    + FETCH_RECV + 8 ))

PRINCPTRS=$(( (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * 4 ))
TIMER_EST=16
NODE_SIZE=$(( 36 + PRINCPTRS + 4 + 4 + 4 + MAX_MESSAGE_SIZE + TIMER_EST + 8 + 8 ))

SCALARS=52   # seqno(8)+last_assigned_*(4+8+8+8+8+8)
VI_EST=256
TIMERS2=32
PERIODS=16
CALLBACKS=16
MISC=9

REPLICA_SIZE=$(( NODE_SIZE + SCALARS + RQUEUE_BOTH + AR_SIZE + CR_SIZE + SR_SIZE
    + STATE_SIZE + VI_EST + TIMERS2 + PERIODS + CALLBACKS
    + NDET_BUF_SIZE + MAX_MESSAGE_SIZE + MISC ))

format_size() {
    awk "BEGIN {printf \"%7d  (%5.1f KB)\", $1, $1/1024}"
}

echo "============================================"
echo " tbft_replica_t — memory breakdown"
echo "============================================"
echo ""
echo "Config:  REPLICAS=$MAX_NUM_REPLICAS  CLIENTS=$MAX_NUM_CLIENTS  f=$MAX_FAULTY"
echo "         WINDOW=$WINDOW_SIZE  CKPT_INT=$CHECKPOINT_INTERVAL  RQUEUE=$RQUEUE_MAX"
echo "         MSG_MAX=$MAX_MESSAGE_SIZE  REPLY_MAX=$MAX_REPLY_SIZE"
echo ""
echo "Derived: AUTH=$AUTH_SIZE  CERT_VALS=$CERT_MAX_VALS  CKPT_SLOTS=$NUM_CKPT_SLOTS"
echo "         PREPARE_MSG=$PREPARE_MSG_SIZE  COMMIT_MSG=$COMMIT_MSG_SIZE"
echo "         CKPT_MSG=$CKPT_MSG_SIZE  REP_MSG_MAX=$REP_MSG_MAX"
echo ""

# Print breakdown table
printf "  %-38s %7s   %5s\n" "Component" "bytes" "KB"
printf "  %-38s %7s   %5s\n" "--------------------------------------" "-------" "-----"

row() {
    local kb
    kb=$(awk "BEGIN {printf \"%.1f\", $2/1024}")
    printf "  %-38s %7d   %6s\n" "$1" "$2" "$kb"
}

row "prepare_cert_t" $PREPARE_CERT_SIZE
row "commit_cert_t" $COMMIT_CERT_SIZE
row "prepared_cert_t" $PREPARED_CERT_SIZE
row "agreement_slice_t" $SLICE_SIZE
row "agreement_region (ar) [${WINDOW_SIZE}x]" $AR_SIZE
row "ckpt_slot_t" $CKPT_SLOT_SIZE
row "checkpoint_region (cr) [${NUM_CKPT_SLOTS}+${MAX_NUM_REPLICAS}]" $CR_SIZE
row "" 0
row "sr: view_change [${MAX_NUM_REPLICAS}x]" $SR_VC_TOTAL
row "sr: vc_ack [${MAX_NUM_REPLICAS}x${MAX_NUM_REPLICAS}]" $SR_VC_ACK_TOTAL
row "sr: new_view" $SR_NV_SLOT
row "sr: new_key" $SR_NK_SLOT
row "sr: request [${MAX_NUM_CLIENTS}x]" $((MAX_NUM_CLIENTS * SR_REQ_SLOT))
row "sr: reply [${MAX_NUM_CLIENTS}x]" $((MAX_NUM_CLIENTS * SR_REP_SLOT))
row "special_region (sr) total" $SR_SIZE
row "" 0
row "rqueue_entry_t" $RQUEUE_ENTRY_SIZE
row "rqueue_t x2 [${RQUEUE_MAX} entries each]" $RQUEUE_BOTH
row "" 0
row "state_t" $STATE_SIZE
row "node_t" $NODE_SIZE
row "view_info_t" $VI_EST
row "" 0
row "ndet_buf" $NDET_BUF_SIZE
row "out_buf" $MAX_MESSAGE_SIZE
row "scalars + timers + callbacks" $((SCALARS + TIMERS2 + PERIODS + CALLBACKS + MISC))

echo ""
echo "  ───────────────────────────────────────────────────────"
row "tbft_replica_t TOTAL" $REPLICA_SIZE
echo ""

# New_key check
NK_BODY=$(( NK_REP_SIZE + (MAX_NUM_REPLICAS - 1) * NK_SLOT_SIZE ))
NK_ALIGNED=$(align8 $NK_BODY)
NK_TOTAL=$(( NK_ALIGNED + SIG_SIZE ))
echo "── New_key wire size ──"
printf "  body  = %d hdr + (%d-1) * %d slot = %d\n" $NK_REP_SIZE $MAX_NUM_REPLICAS $NK_SLOT_SIZE $NK_BODY
printf "  align = %d\n" $NK_ALIGNED
printf "  + sig = %d\n" $NK_TOTAL
printf "  out_buf = %d  " $MAX_MESSAGE_SIZE
if [ $NK_TOTAL -le $MAX_MESSAGE_SIZE ]; then
    echo "OK (+$((MAX_MESSAGE_SIZE - NK_TOTAL)) spare)"
else
    echo "FAIL (short by $((NK_TOTAL - MAX_MESSAGE_SIZE)))"
fi

# Heap estimate
STACK=8192
HEAP_EST=$(( REPLICA_SIZE + STACK + 8192 ))  # +8K for system overhead
HEAP_KB=$(awk "BEGIN {printf \"%.1f\", $HEAP_EST/1024}")
echo ""
echo "── Heap estimate ──"
printf "  replica + stack + overhead = %d bytes (%.1f KB)\n" $HEAP_EST $HEAP_KB
echo "  ESP32-C3 typical free: ~80–100 KB"
if [ $HEAP_EST -le 100000 ]; then
    echo "  => should fit"
else
    echo "  => WARNING: may not fit, try reducing WINDOW or RQUEUE"
fi
