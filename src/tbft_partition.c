#include "tbft_partition.h"
#include "tbft_message.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tbft_ptree";

int tbft_ptree_compute_levels(int num_blocks, int p_children)
{
    /* O3 FIX: Validate p_children to prevent infinite loop or overflow. */
    if (p_children < 2) return TBFT_P_LEVELS + 1; /* invalid: will be caught */

    long nodes_at_level = 1;
    int level = 0;
    while (nodes_at_level < num_blocks) {
        nodes_at_level *= p_children;
        level++;
        if (level > TBFT_P_LEVELS) break; /* will be rejected by caller */
    }
    return level + 1; /* +1 to include root */
}

/* M3 FIX: Use long to prevent integer overflow when computing node counts.
 * With TBFT_P_CHILDREN ≈ 56 and level=3, 56³ = 175,616 fits in int, but
 * larger configs could overflow. Return a sentinel value above any valid
 * bound so callers can detect the overflow. */
int tbft_ptree_nodes_at_level(int level, int p_children)
{
    long n = 1;
    for (int i = 0; i < level; i++) {
        n *= p_children;
        if (n > 1000000) return 1000001; /* sentinel: overflow guard */
    }
    return (int)n;
}

int tbft_ptree_init(tbft_ptree_t *tree, int num_blocks, int p_children)
{
    memset(tree, 0, sizeof(*tree));

    int levels = tbft_ptree_compute_levels(num_blocks, p_children);
    if (levels > TBFT_P_LEVELS) {
        ESP_LOGE(TAG, "need %d levels but TBFT_P_LEVELS=%d; increase or "
                 "reduce state size", levels, TBFT_P_LEVELS);
        return -1;
    }

    tree->dims.p_levels   = levels;
    tree->dims.p_children = p_children;
    tree->dims.num_blocks = num_blocks;

    /* Count total nodes across all levels.
     * Check the sentinel return (1000001) before accumulating to avoid
     * passing a wildly large count to calloc. */
    int total = 0;
    for (int l = 0; l < levels; l++) {
        int n_at_l = tbft_ptree_nodes_at_level(l, p_children);
        if (n_at_l > 1000000) {
            ESP_LOGE(TAG, "ptree: level %d node count overflow", l);
            return -1;
        }
        total += n_at_l;
    }
    tree->total_nodes = total;

    tree->ptree_mem = (tbft_part_t *)calloc((size_t)total, sizeof(tbft_part_t));
    tree->stree_mem = (tbft_dsum_t *)calloc((size_t)total, sizeof(tbft_dsum_t));
    if (!tree->ptree_mem || !tree->stree_mem) {
        ESP_LOGE(TAG, "out of memory for partition tree (%d nodes)", total);
        free(tree->ptree_mem);
        free(tree->stree_mem);
        return -1;
    }

    /* Slice backing arrays into per-level pointers */
    int offset = 0;
    for (int l = 0; l < levels; l++) {
        tree->ptree[l] = tree->ptree_mem + offset;
        tree->stree[l] = tree->stree_mem + offset;
        offset += tbft_ptree_nodes_at_level(l, p_children);
    }

    ESP_LOGI(TAG, "ptree init: %d blocks, %d levels, %d children, %d total nodes",
             num_blocks, levels, p_children, total);
    return 0;
}

void tbft_ptree_free(tbft_ptree_t *tree)
{
    free(tree->ptree_mem);
    free(tree->stree_mem);
    memset(tree, 0, sizeof(*tree));
}

int tbft_ptree_update_leaf(tbft_ptree_t *tree, int block_idx,
                            const tbft_digest_t *block_digest,
                            tbft_seqno_t version)
{
    int levels    = tree->dims.p_levels;
    int pchildren = tree->dims.p_children;
    int leaf_level = levels - 1;

    /* M4 FIX: Validate block_idx against tree dimensions to prevent
     * out-of-bounds array access. This function is exposed in the public
     * header and could be called from unvalidated paths. */
    if (block_idx < 0 || block_idx >= tree->dims.num_blocks) {
        ESP_LOGE(TAG, "update_leaf: block_idx %d out of range [0, %d)",
                 block_idx, tree->dims.num_blocks);
        return -1;
    }
    if (leaf_level < 0 || leaf_level >= TBFT_P_LEVELS) {
        ESP_LOGE(TAG, "update_leaf: invalid leaf_level %d", leaf_level);
        return -1;
    }

    /* Update the leaf */
    tbft_part_t *leaf = &tree->ptree[leaf_level][block_idx];
    leaf->digest  = *block_digest;
    leaf->version = version;

    /* Propagate upward by hashing the concatenation of children's digests.
     * Using concatenate-then-hash (not XOR-then-hash) is required for
     * collision resistance: XOR(d1,d2) == XOR(d3,d4) is easily achievable
     * even if all di are distinct valid digests. */
    int idx = block_idx;
    for (int l = leaf_level; l > 0; l--) {
        int parent_idx = tbft_ptree_parent(idx, pchildren);

        int first_child    = tbft_ptree_first_child(parent_idx, pchildren);
        int num_nodes_at_l = tbft_ptree_nodes_at_level(l, pchildren);

        /* Build parent digest with a multi-part SHA-256 over all child digests */
        psa_hash_operation_t hash_op;
        memset(&hash_op, 0, sizeof(hash_op));
        psa_status_t pst = psa_hash_setup(&hash_op, PSA_ALG_SHA_256);
        if (pst != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_hash_setup failed: %d", (int)pst);
            psa_hash_abort(&hash_op);
            return -1; /* HIGH FIX H5: Abort on hash failure — don't corrupt tree */
        }

        for (int c = first_child;
             c < first_child + pchildren && c < num_nodes_at_l;
             c++) {
            pst = psa_hash_update(&hash_op,
                                  tree->ptree[l][c].digest.bytes,
                                  TBFT_DIGEST_SIZE);
            if (pst != PSA_SUCCESS) {
                ESP_LOGE(TAG, "psa_hash_update failed: %d", (int)pst);
                psa_hash_abort(&hash_op);
                return -1;
            }
        }
        size_t hash_len = 0;
        tbft_part_t *parent = &tree->ptree[l - 1][parent_idx];
        psa_status_t fin = psa_hash_finish(&hash_op, parent->digest.bytes,
                                            TBFT_DIGEST_SIZE, &hash_len);
        if (fin != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_hash_finish failed: %d", (int)fin);
            psa_hash_abort(&hash_op);
            return -1; /* HIGH FIX H5: Abort on hash failure — don't corrupt tree */
        }

        parent->version = version;
        idx = parent_idx;
    }
    return 0;
}
