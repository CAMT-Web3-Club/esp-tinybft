#include "tbft_view_info.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tbft_vi";

void tbft_vi_init(tbft_view_info_t *vi, int num_replicas, int threshold,
                  tbft_special_region_t *sr)
{
    memset(vi, 0, sizeof(*vi));
    vi->num_replicas = num_replicas;
    vi->threshold    = threshold;
    vi->sr           = sr;
}

void tbft_vi_reset(tbft_view_info_t *vi, tbft_view_t new_target_view)
{
    vi->target_view = new_target_view;
    vi->n_received  = 0;
    memset(vi->received,    0, sizeof(vi->received));
    memset(vi->last_stable, 0, sizeof(vi->last_stable));
    if (vi->sr) {
        tbft_sr_clear_vc(vi->sr);
    }
}

bool tbft_vi_collect_vc(tbft_view_info_t *vi,
                        tbft_node_id_t sender_id,
                        const void *msg, int msg_len)
{
    if (sender_id < 0 || sender_id >= TBFT_MAX_NUM_REPLICAS) return false;

    if (msg_len < (int)sizeof(tbft_view_change_rep_t)) return false;
    const tbft_view_change_rep_t *rep = (const tbft_view_change_rep_t *)msg;

    if (rep->v != vi->target_view) {
        if (rep->v > vi->target_view) {
            tbft_vi_reset(vi, rep->v);
            ESP_LOGI(TAG, "advanced target_view to %lld (with reset) from vc of %d",
                     (long long)vi->target_view, sender_id);
        } else {
            ESP_LOGD(TAG, "vc from %d for view %lld, expected %lld",
                     sender_id, (long long)rep->v, (long long)vi->target_view);
            return false;
        }
    }

    if (vi->received[sender_id]) return false;

    if (!tbft_sr_store_vc(vi->sr, sender_id, msg, msg_len)) {
        ESP_LOGW(TAG, "failed to store vc from %d", sender_id);
        return false;
    }

    vi->received[sender_id]    = true;
    vi->last_stable[sender_id] = rep->ls;
    vi->n_received++;

    ESP_LOGD(TAG, "collected vc from %d, view=%lld, ls=%lld, total=%d",
             sender_id, (long long)rep->v, (long long)rep->ls,
             vi->n_received);
    return true;
}

bool tbft_vi_has_quorum(const tbft_view_info_t *vi)
{
    return vi->n_received >= vi->threshold;
}

void tbft_vi_compute_min_max(const tbft_view_info_t *vi,
                             tbft_seqno_t *min_out, tbft_seqno_t *max_out)
{
    tbft_seqno_t min_val = 0;
    tbft_seqno_t max_val = TBFT_WINDOW_SIZE; /* conservative upper bound */
    bool         first   = true;

    for (int i = 0; i < vi->num_replicas; i++) {
        if (!vi->received[i]) continue;

        tbft_seqno_t ls = vi->last_stable[i];
        if (first) {
            min_val = ls;
            max_val = ls + TBFT_WINDOW_SIZE;
            first   = false;
        } else {
            /* min = max of all ls values */
            if (ls > min_val) min_val = ls;
            /* max = min of (ls + window) */
            if (ls + TBFT_WINDOW_SIZE < max_val)
                max_val = ls + TBFT_WINDOW_SIZE;
        }
    }

    if (min_out) *min_out = min_val;
    if (max_out) *max_out = max_val;

    ESP_LOGD(TAG, "compute_min_max: min=%lld max=%lld",
             (long long)min_val, (long long)max_val);
}

bool tbft_vi_verify_nv(const tbft_view_info_t *vi,
                       const tbft_new_view_rep_t *nv, int nv_len)
{
    if (nv_len < (int)sizeof(tbft_new_view_rep_t)) return false;
    if (nv->v != vi->target_view) return false;
    if (nv->min > nv->max) return false;

    /* A New_view must be supported by a quorum (2f+1) of View_change
     * messages.  Without enough VCs, compute_min_max produces lax bounds
     * (min=0, max=WINDOW_SIZE), allowing a buggy or premature New_view
     * to pass verification and corrupt the state machine. */
    if (vi->n_received < vi->threshold) {
        ESP_LOGW(TAG, "verify_nv: only %d VCs received (need %d)",
                 vi->n_received, vi->threshold);
        return false;
    }

    tbft_seqno_t expected_min, expected_max;
    tbft_vi_compute_min_max(vi, &expected_min, &expected_max);

    if (nv->min < expected_min) {
        ESP_LOGW(TAG, "verify_nv: min too small: got %lld, expected >= %lld",
                 (long long)nv->min, (long long)expected_min);
        return false;
    }
    if (nv->max > expected_max) {
        ESP_LOGW(TAG, "verify_nv: max too large: got %lld, expected <= %lld",
                 (long long)nv->max, (long long)expected_max);
        return false;
    }

    /* Verify each prepared proof against collected View_change messages.
     * A Byzantine new primary can inject arbitrary proofs; each
     * (seqno, digest) must appear in at least f+1 collected VCs.
     * n_prep is untrusted — validate it fits the message body first. */
    int32_t proofs_size = (int32_t)((size_t)nv->n_prep * sizeof(tbft_vc_req_info_t));
    int32_t header_size = (int32_t)sizeof(tbft_new_view_rep_t);
    if (proofs_size > 0) {
        if (nv_len < header_size + proofs_size) {
            ESP_LOGW(TAG, "verify_nv: body too short for n_prep=%d (len=%d need=%d)",
                     nv->n_prep, nv_len, (int)(header_size + proofs_size));
            return false;
        }
        const uint8_t *proofs = (const uint8_t *)nv + header_size;
        for (int p = 0; p < nv->n_prep; p++) {
            const tbft_vc_req_info_t *proof =
                (const tbft_vc_req_info_t *)(proofs + (size_t)p * sizeof(tbft_vc_req_info_t));

            int attestations = 0;
            for (int r = 0; r < vi->num_replicas
                 && attestations < (vi->threshold / 2) + 1; r++) {
                if (!vi->received[r]) continue;
                int vc_len = 0;
                const uint8_t *vc_buf = tbft_sr_load_vc(vi->sr, r, &vc_len);
                if (!vc_buf) continue;
                int32_t min_vc_sz = (int32_t)sizeof(tbft_view_change_rep_t)
                                  + (int32_t)sizeof(tbft_sig_t);
                if (vc_len < min_vc_sz) continue;
                const tbft_view_change_rep_t *vc =
                    (const tbft_view_change_rep_t *)vc_buf;

                if (vc->n_ckpts < 0 || vc->n_reqs < 0) continue;
                int32_t ckpt_bytes = (int32_t)((size_t)vc->n_ckpts * sizeof(tbft_vc_ckpt_t));
                int32_t req_bytes  = (int32_t)((size_t)vc->n_reqs * sizeof(tbft_vc_req_info_t));
                /* signature sits at vc_buf + vc_len - sizeof(tbft_sig_t) */
                const uint8_t *body_end =
                    vc_buf + vc_len - (int32_t)sizeof(tbft_sig_t);

                const uint8_t *reqs = vc_buf + sizeof(tbft_view_change_rep_t) + ckpt_bytes;
                if (reqs + req_bytes > body_end) continue;

                for (int q = 0; q < vc->n_reqs; q++) {
                    const tbft_vc_req_info_t *vc_req =
                        (const tbft_vc_req_info_t *)(reqs + (size_t)q * sizeof(tbft_vc_req_info_t));
                    if (vc_req->seqno == proof->seqno &&
                        tbft_digest_equal(&vc_req->digest, &proof->digest)) {
                        attestations++;
                        break;
                    }
                }
            }
            if (attestations < (vi->threshold / 2) + 1) {
                ESP_LOGW(TAG, "verify_nv: proof seqno=%lld has %d attestations (need %d)",
                         (long long)proof->seqno,
                         attestations, (vi->threshold / 2) + 1);
                return false;
            }
        }
    }
    return true;
}

void tbft_vi_collect_vc_ack(tbft_view_info_t *vi,
                             tbft_node_id_t sender,
                             tbft_node_id_t vc_sender,
                             const void *msg, int msg_len)
{
    if (vi->sr) {
        tbft_sr_store_vc_ack(vi->sr, sender, vc_sender, msg, msg_len);
    }
}
