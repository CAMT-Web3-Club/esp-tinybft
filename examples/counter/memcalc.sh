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
APP_STATE_SIZE=4096   # sizeof(counter_state) in main.c

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
CKPT_REP_SIZE=64
CKPT_MSG_SIZE=$(( CKPT_REP_SIZE + AUTH_SIZE ))
PREPARE_REP_SIZE=72
PREPARE_MSG_SIZE=$(( PREPARE_REP_SIZE + AUTH_SIZE ))
COMMIT_REP_SIZE=72
COMMIT_MSG_SIZE=$(( COMMIT_REP_SIZE + AUTH_SIZE ))
PP_MAX_SIZE=$MAX_MESSAGE_SIZE
align8() { echo $(( ($1 + 7) & ~7 )); }

NK_REP_SIZE=32
NK_SLOT_SIZE=$(( 4 + SIG_SIZE ))
# View_change: header + one checkpoint + one entry per window seqno, signed
VC_REP_SIZE=48
VC_CKPT_SIZE=40
VC_REQ_INFO_SIZE=48
VC_MSG_MAX=$(( $(align8 $(( VC_REP_SIZE + VC_CKPT_SIZE + WINDOW_SIZE * VC_REQ_INFO_SIZE ))) + SIG_SIZE ))
# New_view: header + one entry per window seqno, signed
NV_REP_SIZE=80
NV_MSG_MAX=$(( $(align8 $(( NV_REP_SIZE + WINDOW_SIZE * VC_REQ_INFO_SIZE ))) + SIG_SIZE ))
REPLY_REP_SIZE=48
REP_MSG_MAX=$(( $(align8 $(( REPLY_REP_SIZE + MAX_REPLY_SIZE ))) + SIG_SIZE ))
# Client table entry (replicated library state): last rid, seqno, reply
CLIENT_ENTRY_SIZE=$(align8 $(( 8 + 8 + 4 + 4 + MAX_REPLY_SIZE )))
SYS_STATE_SIZE=$(( (MAX_NUM_CLIENTS * CLIENT_ENTRY_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE ))

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
# + seqno tag, commit time, New_view digest, prepared view and digest
SLICE_SIZE=$(( PREPARED_CERT_SIZE + COMMIT_CERT_SIZE + 96 ))

AR_OVERHEAD=24  # head(8)+head_idx(4)+mask(4)+prepare_thr(4)+commit_thr(4)
AR_SIZE=$(( SLICE_SIZE * WINDOW_SIZE + AR_OVERHEAD ))

CKPT_SLOT_SIZE=$(( 8
    + MAX_NUM_REPLICAS * CKPT_MSG_SIZE
    + MAX_NUM_REPLICAS * 4
    + MAX_NUM_REPLICAS
    + 4 + CERT_MAX_VALS * DIGEST_SIZE + CERT_MAX_VALS * 4 + 4 + DIGEST_SIZE ))
CKPT_SLOT_SIZE=$(( (CKPT_SLOT_SIZE + 7) & ~7 ))

CR_SIZE=$(( CKPT_SLOT_SIZE * NUM_CKPT_SLOTS + 8 ))

SR_VC_SLOT=$(sr_slot_size $VC_MSG_MAX)
SR_NV_SLOT=$(sr_slot_size $NV_MSG_MAX)
SR_REP_SLOT=$(sr_slot_size $REP_MSG_MAX)

SR_VC_TOTAL=$(( MAX_NUM_REPLICAS * SR_VC_SLOT ))
SR_OVERHEAD=8  # num_replicas(4)+num_clients(4)

SR_SIZE=$(( SR_VC_TOTAL + SR_NV_SLOT + MAX_NUM_CLIENTS * SR_REP_SLOT + SR_OVERHEAD ))

RQUEUE_ENTRY_SIZE=$(( MAX_MESSAGE_SIZE + 8 ))  # buf(2048)+len(4)+ro(1)+used(1)+pad(2)
RQUEUE_SIZE_ONE=$(( RQUEUE_MAX * RQUEUE_ENTRY_SIZE + 12 ))  # entries + head(4)+tail(4)+count(4)
RQUEUE_BOTH=$(( RQUEUE_SIZE_ONE * 2 ))

COWB_SIZE=$(( (MAX_STATE_BLOCKS / 64 + 1) * 8 ))
PTREE_EST=40
# Partition tree storage (static): every level full, as many levels as
# MAX_STATE_BLOCKS leaves need (capped at P_LEVELS); 40 + 32 bytes per node
P_CHILDREN=$(( (MAX_MESSAGE_SIZE - 40) / 36 ))
if   [ $MAX_STATE_BLOCKS -le 1 ]; then PT_LEVELS=1
elif [ $MAX_STATE_BLOCKS -le $P_CHILDREN ]; then PT_LEVELS=2
elif [ $MAX_STATE_BLOCKS -le $(( P_CHILDREN * P_CHILDREN )) ]; then PT_LEVELS=3
else PT_LEVELS=4; fi
[ $PT_LEVELS -gt $P_LEVELS ] && PT_LEVELS=$P_LEVELS
PT_NODES=1; PT_N=1
for ((l = 1; l < PT_LEVELS; l++)); do PT_N=$(( PT_N * P_CHILDREN )); PT_NODES=$(( PT_NODES + PT_N )); done
PTREE_STORE=$(( PT_NODES * (40 + 32) ))
# Copy of every block at the latest checkpoint (served to fetching replicas)
IMAGE_SIZE=$(( MAX_STATE_BLOCKS * BLOCK_SIZE ))
CKPT_RECORD_SIZE=48
FETCH_QUEUE=$(( MAX_STATE_BLOCKS * 12 ))
FETCH_RECV=$(( ((MAX_STATE_BLOCKS + 63) / 64) * 8 ))

# 136 = pointers, counters, fetch bookkeeping and the fetch target digest
STATE_SIZE=$(( 136 + COWB_SIZE + PTREE_EST + PTREE_STORE + IMAGE_SIZE
    + MAX_STATE_BLOCKS * DIGEST_SIZE
    + NUM_CKPT_SLOTS * CKPT_RECORD_SIZE
    + FETCH_QUEUE + FETCH_RECV ))

PRINCPTRS=$(( (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * 4 ))
# sizeof(tbft_principal_t) with ESP-IDF 6.1 (two mbedtls pk contexts, keys)
PRINCIPAL_SIZE=1712
PRINC_STORE=$(( (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * PRINCIPAL_SIZE ))
TIMER_EST=16
NODE_SIZE=$(( 28 + PRINCPTRS + PRINC_STORE + 4 + 4 + 4 + MAX_MESSAGE_SIZE + TIMER_EST + 8 + 8 ))

VI_EST=$(align8 $(( 32 + MAX_NUM_REPLICAS + 8 * MAX_NUM_REPLICAS )))
# per-principal request-id counters (received, executed, forwarded, assigned)
RID_ARRAYS=$(( 4 * (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * 8 ))
# per-replica view-change and Status tracking, per-client reply times
PEER_ARRAYS=$(align8 $(( 6 * 8 * MAX_NUM_REPLICAS + MAX_NUM_REPLICAS + 8 * MAX_NUM_CLIENTS )))
FIXED=224    # sequence numbers, timers, event group, callbacks, statistics

REPLICA_SIZE=$(( NODE_SIZE + RQUEUE_BOTH + AR_SIZE + CR_SIZE + SR_SIZE
    + STATE_SIZE + VI_EST + NDET_BUF_SIZE + MAX_MESSAGE_SIZE + SYS_STATE_SIZE
    + RID_ARRAYS + PEER_ARRAYS + FIXED ))

STATE_BLOCKS=$(( APP_STATE_SIZE / BLOCK_SIZE + SYS_STATE_SIZE / BLOCK_SIZE ))

# Static memory outside tbft_replica_t
# ESP-NOW transport: state with 8 reassembly slots, receive queue (16
# entries), send queue (8), send task stack (8192) and FreeRTOS objects
ESPNOW_SIZE=$(( 352 + 8 * (MAX_MESSAGE_SIZE + 28) + 16 * (MAX_MESSAGE_SIZE + 12)
    + 8 * (MAX_MESSAGE_SIZE + 8) + 8192 + 600 ))
UDP_SIZE=280
FILE_BUF=$(( 256 + (MAX_NUM_REPLICAS + MAX_NUM_CLIENTS) * 240 ))
[ $FILE_BUF -lt $(( 8 * SIG_SIZE )) ] && FILE_BUF=$(( 8 * SIG_SIZE ))
FILE_BUF=$(( FILE_BUF + 1 ))

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
row "checkpoint_region (cr) [${NUM_CKPT_SLOTS}x]" $CR_SIZE
row "" 0
row "sr: view_change [${MAX_NUM_REPLICAS}x]" $SR_VC_TOTAL
row "sr: new_view" $SR_NV_SLOT
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
row "  in state_t: checkpoint image [${MAX_STATE_BLOCKS}x${BLOCK_SIZE}]" $IMAGE_SIZE
row "  in state_t: partition tree [${PT_NODES} nodes]" $PTREE_STORE
row "  in node_t: principals [$((MAX_NUM_REPLICAS + MAX_NUM_CLIENTS))x${PRINCIPAL_SIZE}]" $PRINC_STORE
row "ndet_buf" $NDET_BUF_SIZE
row "out_buf" $MAX_MESSAGE_SIZE
row "client table [${MAX_NUM_CLIENTS}x${CLIENT_ENTRY_SIZE}, in blocks]" $SYS_STATE_SIZE
row "request-id + peer tracking arrays" $((RID_ARRAYS + PEER_ARRAYS))
row "scalars + timers + callbacks" $FIXED

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

# Static memory of the component (no heap: the linker checks that it fits)
TASK_STACK=8192   # the example's replica/client task, static too
STATIC_ESPNOW=$(( REPLICA_SIZE + ESPNOW_SIZE + FILE_BUF + TASK_STACK ))
STATIC_UDP=$(( REPLICA_SIZE + UDP_SIZE + FILE_BUF + TASK_STACK ))
echo ""
echo "── Static memory (component + example task) ──"
printf "  state blocks used by the counter: %d of %d\n" $STATE_BLOCKS $MAX_STATE_BLOCKS
row "tbft_replica_t (client data shares it)" $REPLICA_SIZE
row "ESP-NOW transport" $ESPNOW_SIZE
row "UDP transport" $UDP_SIZE
row "config/key file buffer" $FILE_BUF
row "role task stack" $TASK_STACK
row "total with ESP-NOW" $STATIC_ESPNOW
row "total with UDP" $STATIC_UDP
echo "  ESP32-C3: about 320 KB of DRAM for all static data; what is left is"
echo "  heap for ESP-IDF itself (Wi-Fi, lwIP, mbedtls). Check with idf.py size."
echo "  ESP32: at most 160 KB of static DRAM (ESP-IDF api-guides/memory-types)."

