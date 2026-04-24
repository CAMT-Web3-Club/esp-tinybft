#pragma once

#include "tbft_types.h"
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Partition tree — Merkle-like digest tree over state blocks (section 8).
 *
 * Layout:
 *   ptree[level][index] → Part (digest + version)
 *   stree[level][index] → DSum (sum of child digests for fast update)
 *
 * With DYNAMIC_PARTITION_TREE, PLevels and PChildren are computed at runtime.
 * Otherwise:
 *   PChildren = TBFT_P_CHILDREN  (from tbft_config.h)
 *   PLevels   = 4                (default)
 *
 * Root is at ptree[0][0].  Leaves at ptree[PLevels-1][i] correspond to
 * individual state blocks.
 * -------------------------------------------------------------------------- */

/* TBFT_P_LEVELS is defined in tbft_config.h (sourced from Kconfig) */

/** One partition node: state digest + checkpoint version */
typedef struct {
    tbft_digest_t  digest;
    tbft_seqno_t   version; /* seqno of last checkpoint that changed this node */
} tbft_part_t;

/** One digest-sum node: XOR/sum of child digests (for incremental update) */
typedef struct {
    tbft_digest_t digest;
} tbft_dsum_t;

/** Partition tree dimensions */
typedef struct {
    int p_levels;   /* number of levels (PLevels) */
    int p_children; /* branching factor (PChildren) */
    int num_blocks; /* number of state blocks (leaf count) */
} tbft_ptree_dims_t;

/**
 * Compute the number of levels needed to cover @p num_blocks leaves.
 */
int tbft_ptree_compute_levels(int num_blocks, int p_children);

/**
 * Number of nodes at a given level:  p_children ^ level
 */
int tbft_ptree_nodes_at_level(int level, int p_children);

/**
 * Return the parent index for node (level, index).
 */
static inline int tbft_ptree_parent(int index, int p_children)
{
    return index / p_children;
}

/**
 * Return the first child index for node (level, index).
 */
static inline int tbft_ptree_first_child(int index, int p_children)
{
    return index * p_children;
}

/* --------------------------------------------------------------------------
 * Partition tree manager
 * -------------------------------------------------------------------------- */

typedef struct {
    tbft_ptree_dims_t dims;

    /* ptree[level] points to an array of (p_children^level) tbft_part_t */
    tbft_part_t  *ptree[TBFT_P_LEVELS];

    /* stree[level] points to an array of (p_children^level) tbft_dsum_t */
    tbft_dsum_t  *stree[TBFT_P_LEVELS];

    /* Backing storage (allocated once, sliced into levels) */
    tbft_part_t  *ptree_mem;
    tbft_dsum_t  *stree_mem;
    int           total_nodes; /* sum of nodes across all levels */
} tbft_ptree_t;

/**
 * Initialise the partition tree for @p num_blocks state blocks.
 * Allocates ptree and stree from the heap.
 * @return 0 on success, -1 on allocation failure
 */
int tbft_ptree_init(tbft_ptree_t *tree, int num_blocks, int p_children);

/**
 * Free heap memory held by the tree.
 */
void tbft_ptree_free(tbft_ptree_t *tree);

/**
 * Update the digest for leaf @p block_idx using the new block digest.
 * Propagates changes up the tree (stree then ptree).
 * @return 0 on success, -1 on hash or validation failure
 */
int tbft_ptree_update_leaf(tbft_ptree_t *tree, int block_idx,
                            const tbft_digest_t *block_digest,
                            tbft_seqno_t version);

/**
 * Return the root digest (overall state digest).
 */
static inline const tbft_digest_t *tbft_ptree_root_digest(
        const tbft_ptree_t *tree)
{
    return &tree->ptree[0][0].digest;
}

/**
 * Return a pointer to ptree node at (level, index).
 */
static inline tbft_part_t *tbft_ptree_node(tbft_ptree_t *tree,
                                           int level, int index)
{
    return &tree->ptree[level][index];
}
