#pragma once

#include "tbft_types.h"
#include "tbft_checkpoint_region.h"
#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * NOTE: The original Log<T> types (tbft_plog_t, tbft_clog_t, tbft_elog_t)
 * defined in this file were superseded by the static memory regions:
 *   - tbft_agreement_region_t  (replaces plog — Pre_prepare/Prepare certs)
 *   - tbft_checkpoint_region_t (replaces clog + elog — Commit/Checkpoint)
 *   - tbft_special_region_t    (view-change, request queues)
 *
 * These old types are no longer used anywhere in the codebase. They are
 * removed to avoid confusion and reduce compilation overhead.
 *
 * Use tbft_agreement_region_t, tbft_checkpoint_region_t, and
 * tbft_special_region_t directly via the replica struct instead.
 * -------------------------------------------------------------------------- */
