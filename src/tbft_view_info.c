#include "tbft_view_info.h"
#include "esp_log.h"
#include <string.h>

static constexpr char TAG[] = "tbft_vi";

void tbft_vi_init(tbft_view_info_t *vi, int num_replicas, int threshold,
                  tbft_special_region_t *sr)
{
    memset(vi, 0, sizeof(*vi));
    vi->num_replicas = num_replicas;
    vi->threshold    = threshold;
    vi->sr           = sr;
}

void tbft_vi_set_target(tbft_view_info_t *vi, tbft_view_t target_view)
{
    vi->target_view = target_view;
    vi->n_received  = 0;
    for (int i = 0; i < vi->num_replicas && i < TBFT_MAX_NUM_REPLICAS; i++) {
        int len = 0;
        const uint8_t *m = vi->sr ? tbft_sr_load_vc(vi->sr, i, &len) : nullptr;
        const tbft_view_change_rep_t *vc = (const tbft_view_change_rep_t *)m;
        vi->received[i] = m && len >= (int)sizeof(*vc) && vc->v == target_view;
        vi->last_stable[i] = vi->received[i] ? vc->ls : 0;
        if (vi->received[i]) vi->n_received++;
    }
}

bool tbft_vi_collect_vc(tbft_view_info_t *vi,
                        tbft_node_id_t sender_id,
                        const void *msg, int msg_len)
{
    if (sender_id < 0 || sender_id >= TBFT_MAX_NUM_REPLICAS) return false;
    if (msg_len < (int)sizeof(tbft_view_change_rep_t)) return false;
    const tbft_view_change_rep_t *rep = (const tbft_view_change_rep_t *)msg;

    /* Keep only the newest View_change of each replica. */
    int old_len = 0;
    const uint8_t *old = tbft_sr_load_vc(vi->sr, sender_id, &old_len);
    if (old && old_len >= (int)sizeof(tbft_view_change_rep_t) &&
        ((const tbft_view_change_rep_t *)old)->v > rep->v) {
        return false;
    }
    if (!tbft_sr_store_vc(vi->sr, sender_id, msg, msg_len)) {
        ESP_LOGW(TAG, "failed to store vc from %d", sender_id);
        return false;
    }
    tbft_vi_set_target(vi, vi->target_view);
    ESP_LOGD(TAG, "stored vc from %d for view %lld (target %lld: %d/%d)",
             sender_id, (long long)rep->v, (long long)vi->target_view,
             vi->n_received, vi->threshold);
    return rep->v == vi->target_view;
}

bool tbft_vi_has_quorum(const tbft_view_info_t *vi)
{
    return vi->n_received >= vi->threshold;
}
