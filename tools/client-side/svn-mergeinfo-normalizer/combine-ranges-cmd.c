/*
 * combine-ranges-cmd.c -- Combine revision ranges in MI if the gap between
 *                         them is inoperative for the respective path.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_cmdline.h"
#include "svn_dirent_uri.h"
#include "svn_hash.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "private/svn_fspath.h"

#include "mergeinfo-normalizer.h"

#include "svn_private_config.h"


/*** Code. ***/

static svn_boolean_t
all_positive_ranges(svn_rangelist_t *ranges)
{
  int i;
  for (i = 0; i < ranges->nelts; ++i)
    {
      const svn_merge_range_t *range
        = APR_ARRAY_IDX(ranges, i, const svn_merge_range_t *);

      if (range->start > range->end)
        return FALSE;
    }

  return TRUE;
}

static svn_boolean_t
inoperative(svn_min__log_t *log,
            const char *path,
            svn_revnum_t start,
            svn_revnum_t end,
            apr_pool_t *scratch_pool)
{
  svn_merge_range_t range = { 0 };
  apr_array_header_t *ranges = apr_array_make(scratch_pool, 1, sizeof(&range));

  range.start = start - 1;
  range.end = end;
  APR_ARRAY_PUSH(ranges, svn_merge_range_t *) = &range;

  return svn_min__operative(log, path, ranges, scratch_pool)->nelts == 0;
}

static svn_error_t *
shorten_lines(apr_array_header_t *wc_mergeinfo,
              svn_min__log_t *log,
              svn_ra_session_t *session,
              svn_min__opt_state_t *opt_state,
              apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_pool_t *iterpool2 = svn_pool_create(scratch_pool);

  int i;
  apr_int64_t removed = 0;
  for (i = 0; i < wc_mergeinfo->nelts; ++i)
    {
      apr_hash_index_t *hi;
      svn_mergeinfo_t mergeinfo = svn_min__get_mergeinfo(wc_mergeinfo, i);

      svn_pool_clear(iterpool);

      for (hi = apr_hash_first(iterpool, mergeinfo);
           hi;
           hi = apr_hash_next(hi))
        {
          int source, dest;
          const char *path = apr_hash_this_key(hi);
          svn_rangelist_t *ranges = apr_hash_this_val(hi);

          if (ranges->nelts < 2 || !all_positive_ranges(ranges))
            continue;

          for (source = 1, dest = 0; source < ranges->nelts; ++source)
            {
              svn_merge_range_t *source_range
                = APR_ARRAY_IDX(ranges, source, svn_merge_range_t *);
              svn_merge_range_t *dest_range
                = APR_ARRAY_IDX(ranges, dest, svn_merge_range_t *);

              svn_pool_clear(iterpool2);

              if (   (source_range->inheritable == dest_range->inheritable)
                  && inoperative(log, path, dest_range->end + 1,
                                 source_range->start, iterpool2))
                {
                  dest_range->end = source_range->end;
                }
              else
                {
                  ++dest;
                  APR_ARRAY_IDX(ranges, dest, svn_merge_range_t *)
                    = source_range;
                }
            }

          removed += ranges->nelts - dest - 1;
          ranges->nelts = dest + 1;
        }

      /* Show progress after every 1000 nodes and after the last one. */
      if (!opt_state->quiet
          && ((i+1) % 1000 == 0 || (i+1) == wc_mergeinfo->nelts))
        SVN_ERR(svn_cmdline_printf(iterpool,
                  _("    Processed %d nodes, removed %s revision ranges.\n"),
                  i+1,
                  apr_psprintf(iterpool, "%" APR_UINT64_T_FMT, removed)));
    }

  svn_pool_destroy(iterpool2);
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_min__combine_ranges(apr_getopt_t *os,
                        void *baton,
                        apr_pool_t *pool)
{
  svn_min__cmd_baton_t *cmd_baton = baton;
  cmd_baton->opt_state->combine_ranges = TRUE;
  SVN_ERR(svn_min__run_command(os, baton, shorten_lines, pool));

  return SVN_NO_ERROR;
}
