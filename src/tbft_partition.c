#include "tbft_partition.h"
#include "tbft_message.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tbft_ptree";

int tbft_ptree_compute_levels(int num_blocks, int p_children)
{
    int nodes_at_level = 1;
    int level          = 0;
    while (nodes_at_level < num_blocks) {
        nodes_at_level *= p_children;
        level++;
    }
    return level + 1; /* +1 to include root */
}

int tbft_ptree_nodes_at_level(int level, int p_children)
{
    int n = 1;
    for (int i = 0; i < level; i++) n *= p_children;
    return n;
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

    /* Count total nodes across all levels */
    int total = 0;
    for (int l = 0; l < levels; l++) {
        total += tbft_ptree_nodes_at_level(l, p_children);
    }
    tree->total_nodes = total;

    tree->ptree_mem = (tbft_part_t *)calloc(total, sizeof(tbft_part_t));
    tree->stree_mem = (tbft_dsum_t *)calloc(total, sizeof(tbft_dsum_t));
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

void tbft_ptree_update_leaf(tbft_ptree_t *tree, int block_idx,
                            const tbft_digest_t *block_digest,
                            int32_t version)
{
    int levels    = tree->dims.p_levels;
    int pchildren = tree->dims.p_children;
    int leaf_level = levels - 1;

    /* Update the leaf */
    tbft_part_t *leaf = &tree->ptree[leaf_level][block_idx];
    leaf->digest  = *block_digest;
    leaf->version = version;

    /* Propagate upward by XOR-summing children digests */
    int idx = block_idx;
    for (int l = leaf_level; l > 0; l--) {
        int parent_idx = tbft_ptree_parent(idx, pchildren);

        /* Recompute stree[l-1][parent_idx] as XOR of children digests */
        tbft_dsum_t *dsum = &tree->stree[l - 1][parent_idx];
        memset(dsum->digest.bytes, 0, TBFT_DIGEST_SIZE);

        int first_child = tbft_ptree_first_child(parent_idx, pchildren);
        int num_nodes_at_l = tbft_ptree_nodes_at_level(l, pchildren);
        for (int c = first_child;
             c < first_child + pchildren && c < num_nodes_at_l;
             c++) {
            const uint8_t *cd = tree->ptree[l][c].digest.bytes;
            for (int b = 0; b < TBFT_DIGEST_SIZE; b++) {
                dsum->digest.bytes[b] ^= cd[b];
            }
        }

        /* Hash the XOR-sum to get the parent's partition digest */
        tbft_part_t *parent = &tree->ptree[l - 1][parent_idx];
        tbft_msg_digest(dsum->digest.bytes, TBFT_DIGEST_SIZE,
                        &parent->digest);
        parent->version = version;

        idx = parent_idx;
    }
}
