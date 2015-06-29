/**
 * @copyright
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
 * @endcopyright
 *
 * @file svnmover.h
 * @brief Concept Demo for Move Tracking and Branching
 */

#ifndef SVNMOVER_H
#define SVNMOVER_H

#include "svn_types.h"
#include "svn_ra.h"

#include "private/svn_branch.h"
#include "private/svn_editor3e.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


typedef struct svnmover_wc_t
{
  apr_pool_t *pool;
  const char *repos_root_url;
  /*const char *anchor_repos_relpath;*/
  svn_revnum_t head_revision;
  svn_revnum_t base_revision;

  svn_ra_session_t *ra_session;
  svn_editor3_t *editor;
  int top_branch_num;
  svn_branch_state_t *base_branch;
  svn_branch_state_t *working_branch;
  svn_client_ctx_t *ctx;

} svnmover_wc_t;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVNMOVER_H */

