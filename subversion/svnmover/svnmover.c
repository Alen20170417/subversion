/*
 * svnmover.c: Subversion Multiple URL Client
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
 *
 */

/*  Multiple URL Command Client

    Combine a list of mv, cp and rm commands on URLs into a single commit.

    How it works: the command line arguments are parsed into an array of
    action structures.  The action structures are interpreted to build a
    tree of operation structures.  The tree of operation structures is
    used to drive an RA commit editor to produce a single commit.

    To build this client, type 'make svnmover' from the root of your
    Subversion source directory.
*/

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <apr_lib.h>

#include "svn_private_config.h"
#include "svn_hash.h"
#include "svn_iter.h"
#include "svn_client.h"
#include "svn_cmdline.h"
#include "svn_config.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_string.h"
#include "svn_subst.h"
#include "svn_utf.h"
#include "svn_version.h"

#include "private/svn_cmdline_private.h"
#include "private/svn_subr_private.h"
#include "private/svn_editor3e.h"
#include "private/svn_ra_private.h"
#include "private/svn_string_private.h"
#include "private/svn_sorts_private.h"

/* Version compatibility check */
static svn_error_t *
check_lib_versions(void)
{
  static const svn_version_checklist_t checklist[] =
    {
      { "svn_client", svn_client_version },
      { "svn_subr",   svn_subr_version },
      { "svn_ra",     svn_ra_version },
      { NULL, NULL }
    };
  SVN_VERSION_DEFINE(my_version);

  return svn_ver_check_list2(&my_version, checklist, svn_ver_equal);
}

static svn_boolean_t quiet = FALSE;

/* Is BRANCH1 the same branch as BRANCH2? Compare by full branch-ids; don't
   require identical branch-instance objects. */
#define BRANCH_IS_SAME_BRANCH(branch1, branch2, scratch_pool) \
  (strcmp(svn_branch_instance_get_id(branch1, scratch_pool), \
          svn_branch_instance_get_id(branch2, scratch_pool)) == 0)

/*  */
__attribute__((format(printf, 1, 2)))
static void
notify(const char *fmt,
       ...)
{
  va_list ap;

  if (! quiet)
    {
      va_start(ap, fmt);
      vprintf(fmt, ap);
      va_end(ap);
      printf("\n");
    }
}

#define SVN_CL__LOG_SEP_STRING \
  "------------------------------------------------------------------------\n"

/* ====================================================================== */

typedef struct mtcc_t
{
  apr_pool_t *pool;
  const char *repos_root_url;
  /*const char *anchor_repos_relpath;*/
  svn_revnum_t head_revision;
  svn_revnum_t base_revision;

  svn_ra_session_t *ra_session;
  svn_editor3_t *editor;
  svn_client_ctx_t *ctx;
} mtcc_t;

static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool);

static svn_error_t *
mtcc_create(mtcc_t **mtcc_p,
            const char *anchor_url,
            svn_revnum_t base_revision,
            apr_hash_t *revprops,
            svn_client_ctx_t *ctx,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  apr_pool_t *mtcc_pool = svn_pool_create(result_pool);
  mtcc_t *mtcc = apr_pcalloc(mtcc_pool, sizeof(*mtcc));
  const char *repos_root_url;
  const char *branch_info_dir = NULL;

  mtcc->pool = mtcc_pool;
  mtcc->ctx = ctx;

  SVN_ERR(svn_client_open_ra_session2(&mtcc->ra_session, anchor_url,
                                      NULL /* wri_abspath */, ctx,
                                      mtcc_pool, scratch_pool));

  SVN_ERR(svn_ra_get_repos_root2(mtcc->ra_session, &mtcc->repos_root_url,
                                 result_pool));
  SVN_ERR(svn_ra_get_latest_revnum(mtcc->ra_session, &mtcc->head_revision,
                                   scratch_pool));

  if (! SVN_IS_VALID_REVNUM(base_revision))
    mtcc->base_revision = mtcc->head_revision;
  else if (base_revision > mtcc->head_revision)
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("No such revision %ld (HEAD is %ld)"),
                             base_revision, mtcc->head_revision);
  else
    mtcc->base_revision = base_revision;

  /* Choose whether to store branching info in a local dir or in revprops.
     (For now, just to exercise the options, we choose local files for
     RA-local and revprops for a remote repo.) */
  SVN_ERR(svn_ra_get_repos_root2(mtcc->ra_session, &repos_root_url,
                                 scratch_pool));
  if (strncmp(repos_root_url, "file://", 7) == 0)
    {
      const char *repos_dir;

      SVN_ERR(svn_uri_get_dirent_from_file_url(&repos_dir, repos_root_url,
                                               scratch_pool));
      branch_info_dir = svn_dirent_join(repos_dir, "branch-info", scratch_pool);
    }

  SVN_ERR(svn_ra_get_commit_editor_ev3(mtcc->ra_session, &mtcc->editor,
                                       revprops,
                                       commit_callback, mtcc,
                                       NULL /*lock_tokens*/, FALSE /*keep_locks*/,
                                       branch_info_dir,
                                       result_pool));
  *mtcc_p = mtcc;
  return SVN_NO_ERROR;
}

static svn_error_t *
mtcc_commit(mtcc_t *mtcc,
            apr_pool_t *scratch_pool)
{
  svn_error_t *err;

#if 0
  /* No changes -> no revision. Easy out */
  if (MTCC_UNMODIFIED(mtcc))
    {
      svn_editor3_abort(mtcc->editor);
      svn_pool_destroy(mtcc->pool);
      return SVN_NO_ERROR;
    }
#endif

#if 0
  const char *session_url;

  SVN_ERR(svn_ra_get_session_url(mtcc->ra_session, &session_url, scratch_pool));

  if (mtcc->root_op->kind != OP_OPEN_DIR)
    {
      const char *name;

      svn_uri_split(&session_url, &name, session_url, scratch_pool);

      if (*name)
        {
          SVN_ERR(mtcc_reparent(session_url, mtcc, scratch_pool));

          SVN_ERR(svn_ra_reparent(mtcc->ra_session, session_url, scratch_pool));
        }
    }
#endif

  err = svn_editor3_complete(mtcc->editor);

  return svn_error_trace(err);
}

typedef enum action_code_t {
  ACTION_DIFF,
  ACTION_DIFF_E,
  ACTION_LOG,
  ACTION_LIST_BRANCHES,
  ACTION_LIST_BRANCHES_R,
  ACTION_BRANCH,
  ACTION_MKBRANCH,
  ACTION_BRANCHIFY,
  ACTION_DISSOLVE,
  ACTION_MERGE,
  ACTION_MV,
  ACTION_MKDIR,
  ACTION_PUT_FILE,
  ACTION_CP,
  ACTION_RM
} action_code_t;

typedef struct action_defn_t {
  enum action_code_t code;
  const char *name;
  int num_args;
} action_defn_t;

static const action_defn_t action_defn[] =
{
  {ACTION_DIFF,             "diff", 2},
  {ACTION_DIFF_E,           "diff-e", 2},
  {ACTION_LOG,              "log", 2},
  {ACTION_LIST_BRANCHES,    "branches", 1},
  {ACTION_LIST_BRANCHES_R,  "ls-br-r", 0},
  {ACTION_BRANCH,           "branch", 2},
  {ACTION_MKBRANCH,         "mkbranch", 1},
  {ACTION_BRANCHIFY,        "branchify", 1},
  {ACTION_DISSOLVE,         "dissolve", 1},
  {ACTION_MERGE,            "merge", 3},
  {ACTION_MV,               "mv", 2},
  {ACTION_MKDIR,            "mkdir", 1},
  {ACTION_PUT_FILE,         "put", 2},
  {ACTION_CP,               "cp", 2},
  {ACTION_RM,               "rm", 1},
};

typedef struct action_t {
  action_code_t action;

  /* revision (copy-from-rev of path[0] for cp) */
  svn_opt_revision_t rev_spec[3];

  /* action    path[0]  path[1]  path[2]
   * ------    -------  -------  -------
   * diff[-e]  left     right
   * ls-br[-r]
   * branch    source   target
   * mkbranch  path
   * branchify path
   * dissolve  path
   * merge     from     to       yca@rev
   * mv        source   target
   * mkdir     target
   * put       src-file target
   * cp        source   target
   * rm        target
   */
  const char *relpath[3];
} action_t;

/* ====================================================================== */

/* Find the deepest branch in the repository of which REVNUM:RRPATH is
 * either the root element or a normal, non-sub-branch element.
 *
 * RRPATH is a repository-relative path. REVNUM is a revision number, or
 * SVN_INVALID_REVNUM meaning the current txn.
 *
 * Return the location of the element in that branch, or with
 * EID=-1 if no element exists there.
 *
 * The result will never be NULL, as every path is within at least the root
 * branch.
 */
static svn_error_t *
find_el_rev_by_rrpath_rev(svn_branch_el_rev_id_t **el_rev_p,
                          svn_editor3_t *editor,
                          svn_revnum_t revnum,
                          const char *rrpath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  if (SVN_IS_VALID_REVNUM(revnum))
    {
      SVN_ERR(svn_editor3_find_el_rev_by_path_rev(el_rev_p,
                                                  editor, rrpath, revnum,
                                                  result_pool, scratch_pool));
    }
  else
    {
      svn_branch_el_rev_id_t *el_rev = apr_palloc(result_pool, sizeof(*el_rev));

      svn_editor3_find_branch_element_by_rrpath(
        &el_rev->branch, &el_rev->eid,
        editor, rrpath, scratch_pool);
      el_rev->rev = SVN_INVALID_REVNUM;
      *el_rev_p = el_rev;
    }
  SVN_ERR_ASSERT(*el_rev_p);
  return SVN_NO_ERROR;
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that it is a subbranch root element for SUBBRANCH.
 * Return "" if SUBBRANCH is null.
 */
static const char *
branch_str(svn_branch_instance_t *subbranch,
           apr_pool_t *result_pool)
{
  if (subbranch)
    return apr_psprintf(result_pool,
                      " (branch %s)",
                      svn_branch_instance_get_id(subbranch, result_pool));
  return "";
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that BRANCH:EID is a subbranch root element.
 * Return "" if the element is not a subbranch root element.
 */
static const char *
subbranch_str(svn_branch_instance_t *branch,
              int eid,
              apr_pool_t *result_pool)
{
  svn_branch_instance_t *subbranch
    = svn_branch_get_subbranch_at_eid(branch, eid, result_pool);

  return branch_str(subbranch, result_pool);
}

/* List all branch instances in FAMILY.
 *
 * If RECURSIVE is true, include branches in nested families.
 */
static svn_error_t *
family_list_branch_instances(svn_branch_revision_root_t *rev_root,
                             svn_branch_family_t *family,
                             svn_boolean_t recursive,
                             svn_boolean_t verbose,
                             apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_instance_t) *bi;

  if (verbose)
    {
      printf("family %d (BIDs %d:%d, EIDs %d:%d)\n",
             family->fid,
             family->first_bid, family->next_bid,
             family->first_eid, family->next_eid);
    }
  else
    {
      printf("branches in family %d:\n",
             family->fid);
    }

  for (SVN_ARRAY_ITER(bi, svn_branch_family_get_branch_instances(
                            rev_root, family, scratch_pool), scratch_pool))
    {
      svn_branch_instance_t *branch = bi->val;
      int eid;

      if (verbose)
        {
          printf("  branch %s bid=%d root=e%d /%s\n",
                 svn_branch_instance_get_id(branch, bi->iterpool),
                 branch->sibling_defn->bid, branch->sibling_defn->root_eid,
                 svn_branch_get_root_rrpath(branch, bi->iterpool));
          for (eid = family->first_eid; eid < family->next_eid; eid++)
            {
              const char *rrpath = svn_branch_get_rrpath_by_eid(branch, eid,
                                                                bi->iterpool);

              if (rrpath)
                {
                  const char *relpath
                    = svn_relpath_skip_ancestor(svn_branch_get_root_rrpath(
                                                  branch, bi->iterpool), rrpath);

                  printf("    e%d %s%s\n",
                         eid, relpath[0] ? relpath : ".",
                         subbranch_str(branch, eid, bi->iterpool));
                }
            }
        }
      else
        {
          printf("  %s /%s\n",
                 svn_branch_instance_get_id(branch, bi->iterpool),
                 svn_branch_get_root_rrpath(branch, bi->iterpool));
        }
    }

  if (recursive)
    {
      SVN_ITER_T(svn_branch_family_t) *fi;

      for (SVN_ARRAY_ITER(fi, svn_branch_family_get_children(
                                family, scratch_pool), scratch_pool))
        {
          SVN_ERR(family_list_branch_instances(rev_root, fi->val, recursive,
                                               verbose, fi->iterpool));
        }
    }

  return SVN_NO_ERROR;
}

/* Options to control how strict the merge is about detecting conflicts.
 *
 * The options affect cases that, depending on the user's preference, could
 * either be considered a conflict or be merged to a deterministic result.
 *
 * The set of options is flexible and may be extended in future.
 */
typedef struct merge_conflict_policy_t
{
  /* Whether to merge delete-vs-delete */
  svn_boolean_t merge_double_delete;
  /* Whether to merge add-vs-add (with same parent/name/content) */
  svn_boolean_t merge_double_add;
  /* Whether to merge reparent-vs-reparent (with same parent) */
  svn_boolean_t merge_double_reparent;
  /* Whether to merge rename-vs-rename (with same name) */
  svn_boolean_t merge_double_rename;
  /* Whether to merge modify-vs-modify (with same content) */
  svn_boolean_t merge_double_modify;
  /* Possible additional controls: */
  /* merge (parent, name, props, text) independently or as a group */
  /* merge (parent, name) independently or as a group */
  /* merge (props, text) independently or as a group */
} merge_conflict_policy_t;

/* Merge the content for one element.
 *
 * If there is no conflict, set *CONFLICT_P to FALSE and *RESULT_P to the
 * merged element; otherwise set *CONFLICT_P to TRUE and *RESULT_P to NULL.
 * Note that *RESULT_P can be null, indicating a deletion.
 *
 * This handles any case where at least one of (SIDE1, SIDE2, YCA) exists.
 */
static void
element_merge(svn_branch_el_rev_content_t **result_p,
              svn_boolean_t *conflict_p,
              int eid,
              svn_branch_el_rev_content_t *side1,
              svn_branch_el_rev_content_t *side2,
              svn_branch_el_rev_content_t *yca,
              const merge_conflict_policy_t *policy,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_boolean_t same1 = svn_branch_el_rev_content_equal(yca, side1,
                                                        scratch_pool);
  svn_boolean_t same2 = svn_branch_el_rev_content_equal(yca, side2,
                                                        scratch_pool);
  svn_boolean_t conflict = FALSE;
  svn_branch_el_rev_content_t *result = NULL;

  if (same1)
    {
      result = side2;
    }
  else if (same2)
    {
      result = side1;
    }
  else if (yca && side1 && side2)
    {
      /* All three sides are different, and all exist */
      result = apr_pmemdup(result_pool, yca, sizeof(*result));

      /* merge the parent-eid */
      if (side1->parent_eid == yca->parent_eid)
        {
          result->parent_eid = side2->parent_eid;
        }
      else if (side2->parent_eid == yca->parent_eid)
        {
          result->parent_eid = side1->parent_eid;
        }
      else if (policy->merge_double_reparent
               && side1->parent_eid == side2->parent_eid)
        {
          SVN_DBG(("e%d double reparent: e%d -> { e%d | e%d }",
                   eid, yca->parent_eid, side1->parent_eid, side2->parent_eid));
          result->parent_eid = side1->parent_eid;
        }
      else
        {
          SVN_DBG(("e%d conflict: parent: e%d -> { e%d | e%d }",
                   eid, yca->parent_eid, side1->parent_eid, side2->parent_eid));
          conflict = TRUE;
        }

      /* merge the name */
      if (strcmp(side1->name, yca->name) == 0)
        {
          result->name = side2->name;
        }
      else if (strcmp(side2->name, yca->name) == 0)
        {
          result->name = side1->name;
        }
      else if (policy->merge_double_rename
               && strcmp(side1->name, side2->name) == 0)
        {
          SVN_DBG(("e%d double rename: %s -> { %s | %s }",
                   eid, yca->name, side1->name, side2->name));
          result->name = side1->name;
        }
      else
        {
          SVN_DBG(("e%d conflict: name: %s -> { %s | %s }",
                   eid, yca->name, side1->name, side2->name));
          conflict = TRUE;
        }

      /* merge the content */
      if (svn_element_content_equal(side1->content, yca->content,
                                         scratch_pool))
        {
          result->content = side2->content;
        }
      else if (svn_element_content_equal(side2->content, yca->content,
                                              scratch_pool))
        {
          result->content = side1->content;
        }
      else if (policy->merge_double_modify
               && svn_element_content_equal(side1->content, side2->content,
                                                 scratch_pool))
        {
          SVN_DBG(("e%d double modify: ... -> { ... | ... }",
                   eid));
          result->content = side1->content;
        }
      else
        {
          /* ### Need not conflict if can merge props and text separately. */

          SVN_DBG(("e%d conflict: content: ... -> { ... | ... }",
                   eid));
          conflict = TRUE;
        }
    }
  else if (! side1 && ! side2)
    {
      /* Double delete (as we assume at least one of YCA/SIDE1/SIDE2 exists) */
      if (policy->merge_double_delete)
        {
          SVN_DBG(("e%d double delete",
                   eid));
          result = side1;
        }
      else
        {
          SVN_DBG(("e%d conflict: delete vs. delete",
                   eid));
          conflict = TRUE;
        }
    }
  else if (side1 && side2)
    {
      /* Double add (as we already handled the case where YCA also exists) */
      if (policy->merge_double_add
          && svn_branch_el_rev_content_equal(side1, side2, scratch_pool))
        {
          SVN_DBG(("e%d double add",
                   eid));
          result = side1;
        }
      else
        {
          SVN_DBG(("e%d conflict: add vs. add (%s)",
                   eid,
                   svn_branch_el_rev_content_equal(side1, side2,
                                                   scratch_pool)
                     ? "same content" : "different content"));
          conflict = TRUE;
        }
    }
  else
    {
      /* The remaining cases must be delete vs. modify */
      SVN_DBG(("e%d conflict: delete vs. modify: %d -> { %d | %d }",
               eid, !!yca, !!side1, !!side2));
      conflict = TRUE;
    }

  *result_p = result;
  *conflict_p = conflict;
}

/* Merge ...
 *
 * Merge any sub-branches in the same way, recursively.
 */
static svn_error_t *
branch_merge_subtree_r(svn_editor3_t *editor,
                       const svn_branch_el_rev_id_t *src,
                       const svn_branch_el_rev_id_t *tgt,
                       const svn_branch_el_rev_id_t *yca,
                       apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_yca_src, *diff_yca_tgt;
  svn_boolean_t had_conflict = FALSE;
  int first_eid, next_eid, eid;
  const merge_conflict_policy_t policy = { TRUE, TRUE, TRUE, TRUE, TRUE };

  SVN_ERR_ASSERT(src->branch->sibling_defn->family->fid
                 == tgt->branch->sibling_defn->family->fid);
  SVN_ERR_ASSERT(src->branch->sibling_defn->family->fid
                 == yca->branch->sibling_defn->family->fid);
  SVN_ERR_ASSERT(src->eid == tgt->eid);
  SVN_ERR_ASSERT(src->eid == yca->eid);

  SVN_DBG(("merge src: r%2ld f%d b%2d e%3d",
           src->rev, src->branch->sibling_defn->family->fid,
           src->branch->sibling_defn->bid, src->eid));
  SVN_DBG(("merge tgt: r%2ld f%d b%2d e%3d",
           tgt->rev, tgt->branch->sibling_defn->family->fid,
           tgt->branch->sibling_defn->bid, tgt->eid));
  SVN_DBG(("merge yca: r%2ld f%d b%2d e%3d",
           yca->rev, yca->branch->sibling_defn->family->fid,
           yca->branch->sibling_defn->bid, yca->eid));

  /*
      for (eid, diff1) in element_differences(YCA, FROM):
        diff2 = element_diff(eid, YCA, TO)
        if diff1 and diff2:
          result := element_merge(diff1, diff2)
        elif diff1:
          result := diff1.right
        # else no change
   */
  SVN_ERR(svn_branch_subtree_differences(&diff_yca_src,
                                         editor, yca, src,
                                         scratch_pool, scratch_pool));
  /* ### We only need to query for YCA:TO differences in elements that are
         different in YCA:FROM, but right now we ask for all differences. */
  SVN_ERR(svn_branch_subtree_differences(&diff_yca_tgt,
                                         editor, yca, tgt,
                                         scratch_pool, scratch_pool));

  first_eid = yca->branch->sibling_defn->family->first_eid;
  next_eid = yca->branch->sibling_defn->family->next_eid;
  next_eid = MAX(next_eid, src->branch->sibling_defn->family->next_eid);
  next_eid = MAX(next_eid, tgt->branch->sibling_defn->family->next_eid);

  for (eid = first_eid; eid < next_eid; eid++)
    {
      svn_branch_el_rev_content_t **e_yca_src
        = apr_hash_get(diff_yca_src, &eid, sizeof(eid));
      svn_branch_el_rev_content_t **e_yca_tgt
        = apr_hash_get(diff_yca_tgt, &eid, sizeof(eid));
      svn_branch_el_rev_content_t *e_yca;
      svn_branch_el_rev_content_t *e_src;
      svn_branch_el_rev_content_t *e_tgt;
      svn_branch_el_rev_content_t *result;
      svn_boolean_t conflict;

      /* If an element hasn't changed in the source branch, there is
         no need to do anything with it in the target branch. We could
         use element_merge() for any case where at least one of (SRC,
         TGT, YCA) exists, but we choose to skip it when SRC == YCA. */
      if (! e_yca_src)
        {
          continue;
        }

      e_yca = e_yca_src[0];
      e_src = e_yca_src[1];
      e_tgt = e_yca_tgt ? e_yca_tgt[1] : e_yca_src[0];

      element_merge(&result, &conflict,
                    eid, e_src, e_tgt, e_yca,
                    &policy,
                    scratch_pool, scratch_pool);

      if (conflict)
        {
          notify("!    e%d <conflict>", eid);
          had_conflict = TRUE;
        }
      else if (e_tgt && result)
        {
          notify("M/V  e%d %s%s",
                 eid, result->name,
                 subbranch_str(tgt->branch, eid, scratch_pool));

          SVN_ERR(svn_editor3_alter(editor, tgt->rev, tgt->branch, eid,
                                    result->parent_eid, result->name,
                                    result->content));
        }
      else if (e_tgt)
        {
          notify("D    e%d %s%s",
                 eid, e_yca->name,
                 subbranch_str(yca->branch, eid, scratch_pool));
          SVN_ERR(svn_editor3_delete(editor, tgt->rev, tgt->branch, eid));
        }
      else if (result)
        {
          svn_branch_instance_t *subbranch
            = svn_branch_get_subbranch_at_eid(src->branch, eid, scratch_pool);

          if (subbranch)
            notify("A    e%d %s%s",
                   eid, result->name,
                   subbranch_str(src->branch, eid, scratch_pool));
          else
            notify("A    e%d %s", eid, result->name);

          /* In BRANCH, create an instance of the element EID with new content.
           *
           * Translated to old language, this means create a new node-copy
           * copied (branched) from the source-right version of the merge
           * (which is not specified here, but will need to be),
           * which may be in this branch or in another branch.
           */
          SVN_ERR(svn_editor3_instantiate(editor, tgt->branch, eid,
                                          result->parent_eid, result->name,
                                          result->content));

          if (subbranch)
            {
              SVN_ERR(svn_branch_branch_subtree_r2(
                        NULL,
                        subbranch, subbranch->sibling_defn->root_eid,
                        tgt->branch, eid,
                        subbranch->sibling_defn,
                        scratch_pool));
            }
        }
    }

  if (had_conflict)
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("Merge failed: conflict(s) occurred"));
    }
  else
    {
      SVN_DBG(("merge completed: no conflicts"));
    }

  /* ### TODO: subbranches */

  return SVN_NO_ERROR;
}

/* Merge SRC into TGT, using the common ancestor YCA.
 *
 * Merge the two sets of changes: YCA -> SRC and YCA -> TGT, applying
 * the result to the transaction at TGT.
 *
 * If conflicts arise, just fail.
 *
 * SRC->BRANCH, TGT->BRANCH and YCA->BRANCH must be in the same family.
 *
 * SRC, TGT and YCA must be existing and corresponding (same EID) elements
 * of the branch family.
 *
 * None of SRC, TGT and YCA is a subbranch root element.
 *
 * ### TODO:
 *     If ... contains nested subbranches, these will also be merged.
 */
static svn_error_t *
svn_branch_merge(svn_editor3_t *editor,
                 svn_branch_el_rev_id_t *src,
                 svn_branch_el_rev_id_t *tgt,
                 svn_branch_el_rev_id_t *yca,
                 apr_pool_t *scratch_pool)
{
  if (src->branch->sibling_defn->family->fid != tgt->branch->sibling_defn->family->fid
      || src->branch->sibling_defn->family->fid != yca->branch->sibling_defn->family->fid)
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                             _("Merge branches must all be in same family "
                               "(from: f%d, to: f%d, yca: f%d)"),
                             src->branch->sibling_defn->family->fid,
                             tgt->branch->sibling_defn->family->fid,
                             yca->branch->sibling_defn->family->fid);

  /*SVN_ERR(verify_exists_in_branch(from, scratch_pool));*/
  /*SVN_ERR(verify_exists_in_branch(to, scratch_pool));*/
  /*SVN_ERR(verify_exists_in_branch(yca, scratch_pool));*/
  if (src->eid != tgt->eid || src->eid != yca->eid)
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                             _("Merge branches must all be same element "
                               "(from: e%d, to: e%d, yca: e%d)"),
                             src->eid, tgt->eid, yca->eid);
  /*SVN_ERR(verify_not_subbranch_root(from, scratch_pool));*/
  /*SVN_ERR(verify_not_subbranch_root(to, scratch_pool));*/
  /*SVN_ERR(verify_not_subbranch_root(yca, scratch_pool));*/

  SVN_ERR(branch_merge_subtree_r(editor, src, tgt, yca, scratch_pool));

  return SVN_NO_ERROR;
}

/* Display differences, referring to elements */
static svn_error_t *
svn_branch_diff_e(svn_editor3_t *editor,
                  svn_branch_el_rev_id_t *left,
                  svn_branch_el_rev_id_t *right,
                  const char *prefix,
                  const char *header,
                  apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_yca_tgt;
  int first_eid, next_eid, eid;
  svn_boolean_t printed_header = FALSE;

  if (left->branch->sibling_defn->family->fid
      != right->branch->sibling_defn->family->fid)
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("Left and right side of an element-based diff "
                                 "must be in the same branch family "
                                 "(left: f%d, right: f%d)"),
                               left->branch->sibling_defn->family->fid,
                               right->branch->sibling_defn->family->fid);
    }
  SVN_ERR_ASSERT(left->eid >= 0 && right->eid >= 0);

  SVN_ERR(svn_branch_subtree_differences(&diff_yca_tgt,
                                         editor, left, right,
                                         scratch_pool, scratch_pool));

  first_eid = left->branch->sibling_defn->family->first_eid;
  next_eid = MAX(left->branch->sibling_defn->family->next_eid,
                 right->branch->sibling_defn->family->next_eid);

  for (eid = first_eid; eid < next_eid; eid++)
    {
      svn_branch_el_rev_content_t **e_pair
        = apr_hash_get(diff_yca_tgt, &eid, sizeof(eid));
      svn_branch_el_rev_content_t *e0, *e1;

      if (! e_pair)
        continue;

      e0 = e_pair[0];
      e1 = e_pair[1];

      if (e0 || e1)
        {
          char status_mod = ' ', status_reparent = ' ', status_rename = ' ';

          if (e0 && e1)
            {
              status_mod = 'M';
              status_reparent = (e0->parent_eid != e1->parent_eid) ? 'v' : ' ';
              status_rename  = (strcmp(e0->name, e1->name) != 0) ? 'r' : ' ';
            }
          else
            {
              status_mod = e0 ? 'D' : 'A';
            }

          if (header && ! printed_header)
            {
              printf("%s%s", prefix, header);
              printed_header = TRUE;
            }

          printf("%s%c%c%c e%d  %s%s%s%s\n",
                 prefix,
                 status_mod, status_reparent, status_rename,
                 eid,
                 e1 ? apr_psprintf(scratch_pool, "e%d/%s",
                                   e1->parent_eid, e1->name) : "",
                 subbranch_str(e0 ? left->branch : right->branch, eid,
                               scratch_pool),
                 e0 && e1 ? " from " : "",
                 e0 ? apr_psprintf(scratch_pool, "e%d/%s",
                                   e0->parent_eid, e0->name) : "");
        }
    }

  return SVN_NO_ERROR;
}

/*  */
typedef struct diff_item_t
{
  char status_mod, status_reparent, status_rename;
  const char *major_path;
  const char *from;
  const char *subbranch_str;
} diff_item_t;

/*  */
static int
diff_ordering(const void *a, const void *b)
{
  const diff_item_t *item1 = *(void *const *)a, *item2 = *(void *const *)b;

  /* Sort items with status 'D' before all others */
  if ((item1->status_mod == 'D') != (item2->status_mod == 'D'))
    return (item2->status_mod == 'D') - (item1->status_mod == 'D');

  /* Sort by path */
  return svn_path_compare_paths(item1->major_path, item2->major_path);
}

/* Display differences, referring to paths */
static svn_error_t *
svn_branch_diff(svn_editor3_t *editor,
                svn_branch_el_rev_id_t *left,
                svn_branch_el_rev_id_t *right,
                const char *prefix,
                const char *header,
                apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_yca_tgt;
  int first_eid, next_eid, eid;
  svn_array_t *diff_changes = svn_array_make(scratch_pool);
  SVN_ITER_T(diff_item_t) *ai;

  if (left->branch->sibling_defn->family->fid
      != right->branch->sibling_defn->family->fid)
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("Left and right side of an element-based diff "
                                 "must be in the same branch family "
                                 "(left: f%d, right: f%d)"),
                               left->branch->sibling_defn->family->fid,
                               right->branch->sibling_defn->family->fid);
    }
  SVN_ERR_ASSERT(left->eid >= 0 && right->eid >= 0);

  SVN_ERR(svn_branch_subtree_differences(&diff_yca_tgt,
                                         editor, left, right,
                                         scratch_pool, scratch_pool));

  first_eid = left->branch->sibling_defn->family->first_eid;
  next_eid = MAX(left->branch->sibling_defn->family->next_eid,
                 right->branch->sibling_defn->family->next_eid);

  for (eid = first_eid; eid < next_eid; eid++)
    {
      svn_branch_el_rev_content_t **e_pair
        = apr_hash_get(diff_yca_tgt, &eid, sizeof(eid));
      svn_branch_el_rev_content_t *e0, *e1;

      if (! e_pair)
        continue;

      e0 = e_pair[0];
      e1 = e_pair[1];

      if (e0 || e1)
        {
          diff_item_t *item = apr_palloc(scratch_pool, sizeof(*item));
          const char *path0 = NULL, *path1 = NULL;
          const char *from = "";

          item->status_mod = ' ';
          item->status_reparent = ' ';
          item->status_rename = ' ';

          if (e0 && e1)
            {
              item->status_mod = 'M';
              item->status_reparent = (e0->parent_eid != e1->parent_eid) ? 'v' : ' ';
              item->status_rename  = (strcmp(e0->name, e1->name) != 0) ? 'r' : ' ';
            }
          else
            {
              item->status_mod = e0 ? 'D' : 'A';
            }
          if (e0)
            path0 = svn_branch_get_path_by_eid(left->branch, eid, scratch_pool);
          if (e1)
            path1 = svn_branch_get_path_by_eid(right->branch, eid, scratch_pool);
          if (e0 && e1
              && (e0->parent_eid != e1->parent_eid
                  || strcmp(e0->name, e1->name) != 0))
            {
              if (e0->parent_eid == e1->parent_eid)
                from = apr_psprintf(scratch_pool,
                                    " (renamed from .../%s)",
                                    e0->name);
              else if (strcmp(e0->name, e1->name) == 0)
                from = apr_psprintf(scratch_pool,
                                    " (moved from %s/...)",
                                    svn_branch_get_path_by_eid(left->branch,
                                                               e0->parent_eid,
                                                               scratch_pool));
              else
                from = apr_psprintf(scratch_pool,
                                    " (moved+renamed from %s)",
                                    path0);
            }
          item->major_path = (e1 ? path1 : path0);
          item->from = from;
          item->subbranch_str = subbranch_str(e0 ? left->branch : right->branch,
                                              eid, scratch_pool);
          SVN_ARRAY_PUSH(diff_changes) = item;
        }
    }

  if (header && diff_changes->nelts)
    printf("%s%s", prefix, header);

  for (SVN_ARRAY_ITER_SORTED(ai, diff_changes, diff_ordering, scratch_pool))
    {
      diff_item_t *item = ai->val;

      printf("%s%c%c%c %s%s%s\n",
             prefix,
             item->status_mod, item->status_reparent, item->status_rename,
             item->major_path,
             item->subbranch_str,
             item->from);
    }

  return SVN_NO_ERROR;
}

/* Return a hash of (BID -> BRANCH) of the subbranches of BRANCH.
 *
 * ### Wrong, because BID is not a unique identifier
 *
 * Return an empty hash if BRANCH is null.
 */
static apr_hash_t *
get_subbranches(svn_branch_instance_t *branch,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  apr_hash_t *result = apr_hash_make(result_pool);

  if (branch)
    {
      SVN_ITER_T(svn_branch_instance_t) *bi;

      for (SVN_ARRAY_ITER(bi, svn_branch_get_all_sub_branches(
                                branch, result_pool, scratch_pool),
                          scratch_pool))
        {
          svn_branch_instance_t *b = bi->val;
          int *bid = apr_pmemdup(result_pool, &b->sibling_defn->bid, sizeof (*bid));

          apr_hash_set(result, bid, sizeof (*bid), b);
        }
    }
  return result;
}

typedef svn_error_t *
svn_branch_diff_func_t(svn_editor3_t *editor,
                       svn_branch_el_rev_id_t *left,
                       svn_branch_el_rev_id_t *right,
                       const char *prefix,
                       const char *header,
                       apr_pool_t *scratch_pool);

/* Display differences, referring to paths, recursing into sub-branches */
static svn_error_t *
svn_branch_diff_r(svn_editor3_t *editor,
                  svn_branch_el_rev_id_t *left,
                  svn_branch_el_rev_id_t *right,
                  svn_branch_diff_func_t diff_func,
                  const char *prefix,
                  apr_pool_t *scratch_pool)
{
  const char *header;
  apr_hash_t *subbranches_l, *subbranches_r, *subbranches_all;
  apr_hash_index_t *hi;

  if (!left)
    {
      header = apr_psprintf(scratch_pool,
                 "--- added branch %s, family %d, at /%s\n",
                 svn_branch_instance_get_id(right->branch, scratch_pool),
                 right->branch->sibling_defn->family->fid,
                 svn_branch_get_root_rrpath(right->branch, scratch_pool));
      printf("%s%s", prefix, header);
    }
  else if (!right)
    {
      header = apr_psprintf(scratch_pool,
                 "--- deleted branch %s, family %d, at /%s\n",
                 svn_branch_instance_get_id(left->branch, scratch_pool),
                 left->branch->sibling_defn->family->fid,
                 svn_branch_get_root_rrpath(left->branch, scratch_pool));
      printf("%s%s", prefix, header);
    }
  else
    {
      assert(BRANCH_IS_SAME_BRANCH(left->branch, right->branch, scratch_pool));
      header = apr_psprintf(scratch_pool,
                 "--- diff branch %s, family %d, at /%s : /%s\n",
                 svn_branch_instance_get_id(left->branch, scratch_pool),
                 right->branch->sibling_defn->family->fid,
                 svn_branch_get_root_rrpath(left->branch, scratch_pool),
                 svn_branch_get_root_rrpath(right->branch, scratch_pool));
      SVN_ERR(diff_func(editor, left, right, prefix, header, scratch_pool));
    }

  subbranches_l = get_subbranches(left ? left->branch : NULL,
                                  scratch_pool, scratch_pool);
  subbranches_r = get_subbranches(right ? right->branch : NULL,
                                  scratch_pool, scratch_pool);
  subbranches_all = apr_hash_overlay(scratch_pool,
                                     subbranches_l, subbranches_r);

  for (hi = apr_hash_first(scratch_pool, subbranches_all);
       hi; hi = apr_hash_next(hi))
    {
      int bid = *(const int *)apr_hash_this_key(hi);
      svn_branch_instance_t *branch_l = apr_hash_get(subbranches_l, &bid, sizeof(bid));
      svn_branch_instance_t *branch_r = apr_hash_get(subbranches_r, &bid, sizeof(bid));
      svn_branch_el_rev_id_t *sub_left = NULL, *sub_right = NULL;

      if (branch_l)
        {
          sub_left = svn_branch_el_rev_id_create(branch_l,
                                                 branch_l->sibling_defn->root_eid,
                                                 left->rev,
                                                 scratch_pool);
        }
      if (branch_r)
        {
          sub_right = svn_branch_el_rev_id_create(branch_r,
                                                  branch_r->sibling_defn->root_eid,
                                                  right->rev,
                                                  scratch_pool);
        }

      /* recurse */
      svn_branch_diff_r(editor, sub_left, sub_right, diff_func, prefix,
                        scratch_pool);
    }
  return SVN_NO_ERROR;
}

/* Move in the 'best' way possible.
 *
 *    if target is in same branch:
 *      move the element
 *    else if target is in another branch of same family:
 *      delete element from source branch
 *      instantiate same element in target branch
 *    else:
 *      delete element from source branch
 *      create a new element in target branch
 */
static svn_error_t *
do_move(svn_editor3_t *editor,
        svn_branch_el_rev_id_t *el_rev,
        svn_branch_el_rev_id_t *to_parent_el_rev,
        const char *to_name,
        apr_pool_t *scratch_pool)
{
  svn_branch_el_rev_content_t *old_node;

  /* Simple move/rename within same branch, if possible */
  if (to_parent_el_rev->branch == el_rev->branch)
    {
      /* Move within same branch */
      SVN_ERR(svn_editor3_alter(editor, el_rev->rev,
                                el_rev->branch, el_rev->eid,
                                to_parent_el_rev->eid, to_name,
                                NULL /* "no change" */));
      return SVN_NO_ERROR;
    }

  /* Instantiate same element in another branch of same family, if possible */
  if (el_rev->branch->sibling_defn->family->fid
      == to_parent_el_rev->branch->sibling_defn->family->fid)
    {
      /* Does this element already exist in the target branch? We can't
         use this method if it does. */
      SVN_ERR(svn_editor3_el_rev_get(&old_node,
                                     editor,
                                     to_parent_el_rev->branch, el_rev->eid,
                                     scratch_pool, scratch_pool));
      if (! old_node)
        {
          /* (There is no danger of creating a cyclic directory hierarchy in
             the target branch, as this element doesn't yet exist there.) */

          printf("mv: moving by deleting element in source branch and "
                 "instantiating same element in target branch\n");

          /* Get the old content of the source node (which we know exists) */
          SVN_ERR(svn_editor3_el_rev_get(&old_node,
                                         editor, el_rev->branch, el_rev->eid,
                                         scratch_pool, scratch_pool));
          SVN_ERR_ASSERT(old_node);
          SVN_ERR(svn_editor3_delete(editor, el_rev->rev,
                                     el_rev->branch, el_rev->eid));
          SVN_ERR(svn_editor3_instantiate(editor,
                                          to_parent_el_rev->branch, el_rev->eid,
                                          to_parent_el_rev->eid, to_name,
                                          old_node->content));
          /* ###  We need to move nested branches too. */
          return SVN_NO_ERROR;
        }
    }

  /* Move by copy-and-delete */
  if (el_rev->branch->sibling_defn->family->fid
      != to_parent_el_rev->branch->sibling_defn->family->fid) /* ### always */
    {
      printf("mv: moving by copy-and-delete to a different branch family\n");
    }
  else /* ### never */
    {
      printf("mv: moving by copy-and-delete\n");
    }
  SVN_ERR(svn_editor3_el_rev_get(&old_node,
                                 editor, el_rev->branch, el_rev->eid,
                                 scratch_pool, scratch_pool));
  SVN_ERR(svn_editor3_delete(editor, el_rev->rev,
                             el_rev->branch, el_rev->eid));
  SVN_ERR(svn_editor3_add(editor, NULL /*new_eid*/,
                          old_node->content->kind,
                          to_parent_el_rev->branch,
                          to_parent_el_rev->eid, to_name,
                          old_node->content));

  return SVN_NO_ERROR;
}

/*  */
static svn_branch_instance_t *
svn_branch_revision_root_find_branch_by_id(const svn_branch_revision_root_t *rev_root,
                                           const char *branch_instance_id,
                                           apr_pool_t *scratch_pool)
{
  svn_branch_instance_t *branch = rev_root->root_branch;
  int pos = 1;

  SVN_ERR_ASSERT_NO_RETURN(branch_instance_id[0] == '^');
  while (branch_instance_id[pos])
    {
      int n, eid, bytes;
      n = sscanf(branch_instance_id + pos, ".%d%n", &eid, &bytes);
      SVN_ERR_ASSERT_NO_RETURN(n == 1);

      branch = svn_branch_get_subbranch_at_eid(branch, eid, scratch_pool);
      pos += bytes;
    }
  SVN_DBG(("branch found: f%db%de%d at '/%s'",
           branch->sibling_defn->family->fid,
           branch->sibling_defn->bid,
           branch->sibling_defn->root_eid,
           svn_branch_get_root_rrpath(branch, scratch_pool)));
  return branch;
}

/*  */
static svn_branch_el_rev_id_t *
svn_branch_find_predecessor_el_rev(svn_branch_el_rev_id_t *old_el_rev,
                                   apr_pool_t *result_pool)
{
  const svn_branch_repos_t *repos = old_el_rev->branch->rev_root->repos;
  const svn_branch_revision_root_t *rev_root;
  const char *branch_id;
  svn_branch_instance_t *branch;
  svn_branch_el_rev_id_t *new_el_rev;

  if (old_el_rev->rev <= 0)
    return NULL;

  branch_id = svn_branch_instance_get_id(old_el_rev->branch, result_pool);
  rev_root = svn_array_get(repos->rev_roots, old_el_rev->rev - 1);
  branch = svn_branch_revision_root_find_branch_by_id(rev_root, branch_id,
                                                      result_pool);

  new_el_rev = svn_branch_el_rev_id_create(branch, old_el_rev->eid,
                                           old_el_rev->rev - 1,
                                           result_pool);
  return new_el_rev;
}

/* Similar to 'svn log -v', this iterates over the revisions between
 * LEFT and RIGHT (currently excluding LEFT), printing a single-rev diff
 * for each.
 */
static svn_error_t *
svn_branch_log(svn_editor3_t *editor,
               svn_branch_el_rev_id_t *left,
               svn_branch_el_rev_id_t *right,
               apr_pool_t *scratch_pool)
{
  svn_revnum_t first_rev = left->rev, rev;

  for (rev = right->rev; rev > first_rev; rev--)
    {
      svn_branch_el_rev_id_t *el_rev_left
        = svn_branch_find_predecessor_el_rev(right, scratch_pool);

      printf(SVN_CL__LOG_SEP_STRING "r%ld | ...\n",
             rev);
      printf("Changed elements:\n");
      SVN_ERR(svn_branch_diff_r(editor,
                                el_rev_left, right,
                                svn_branch_diff, "   ",
                                scratch_pool));
      right = el_rev_left;
    }

  return SVN_NO_ERROR;
}

/* This commit callback prints not only a commit summary line but also
 * a log-style summary of the changes.
 */
static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool)
{
  mtcc_t *mtcc = baton;
  svn_branch_el_rev_id_t *el_rev_left, *el_rev_right;
  const char *rrpath = "";

  SVN_ERR(svn_cmdline_printf(pool, "r%ld committed by %s at %s\n",
                             commit_info->revision,
                             (commit_info->author
                              ? commit_info->author : "(no author)"),
                             commit_info->date));

  SVN_ERR(find_el_rev_by_rrpath_rev(&el_rev_left, mtcc->editor,
                                    commit_info->revision - 1, rrpath,
                                    pool, pool));
  SVN_ERR(find_el_rev_by_rrpath_rev(&el_rev_right, mtcc->editor,
                                    commit_info->revision, rrpath,
                                    pool, pool));
  printf("   Committed change:\n");
  SVN_ERR(svn_branch_diff_r(mtcc->editor,
                            el_rev_left, el_rev_right,
                            svn_branch_diff_e, "   ",
                            pool));
  return SVN_NO_ERROR;
}

#define VERIFY_REV_SPECIFIED(op, i)                                     \
  if (el_rev[i]->rev == SVN_INVALID_REVNUM)                             \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s': revision number required"),   \
                             op, action->relpath[i]);

#define VERIFY_REV_UNSPECIFIED(op, i)                                   \
  if (el_rev[i]->rev != SVN_INVALID_REVNUM)                             \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s@...': revision number not allowed"), \
                             op, action->relpath[i]);

#define VERIFY_EID_NONEXISTENT(op, i)                                   \
  if (el_rev[i]->eid != -1)                                             \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Path '%s' already exists"),         \
                             op, action->relpath[i]);

#define VERIFY_EID_EXISTS(op, i)                                        \
  if (el_rev[i]->eid == -1)                                             \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Path '%s' not found"),              \
                             op, action->relpath[i]);

#define VERIFY_PARENT_EID_EXISTS(op, i)                                 \
  if (parent_el_rev[i]->eid == -1)                                      \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Path '%s' not found"),              \
                             op, svn_relpath_dirname(action->relpath[i], pool));

#define is_branch_root_element(branch, eid) \
  ((branch)->sibling_defn->root_eid == (eid))

static svn_error_t *
execute(const apr_array_header_t *actions,
        const char *anchor_url,
        apr_hash_t *revprops,
        svn_revnum_t base_revision,
        svn_client_ctx_t *ctx,
        apr_pool_t *pool)
{
  mtcc_t *mtcc;
  svn_editor3_t *editor;
  const char *base_relpath;
  apr_pool_t *iterpool = svn_pool_create(pool);
  svn_boolean_t made_changes = FALSE;
  int i;
  svn_error_t *err;

  SVN_ERR(mtcc_create(&mtcc,
                      anchor_url, base_revision, revprops,
                      ctx, pool, iterpool));
  editor = mtcc->editor;
  base_relpath = svn_uri_skip_ancestor(mtcc->repos_root_url, anchor_url, pool);
  base_revision = mtcc->base_revision;

  for (i = 0; i < actions->nelts; ++i)
    {
      action_t *action = APR_ARRAY_IDX(actions, i, action_t *);
      int j;
      svn_revnum_t revnum[3] = { -1, -1, -1 };
      const char *path_name[3] = { NULL, NULL, NULL };
      svn_branch_el_rev_id_t *el_rev[3], *parent_el_rev[3];

      svn_pool_clear(iterpool);

      /* Before translating paths to/from elements, need a sequence point */
      svn_editor3_sequence_point(editor);

      for (j = 0; j < 3; j++)
        {
          if (action->relpath[j])
            {
              const char *rrpath, *parent_rrpath;

              if (action->rev_spec[j].kind == svn_opt_revision_unspecified)
                revnum[j] = SVN_INVALID_REVNUM;
              else if (action->rev_spec[j].kind == svn_opt_revision_number)
                revnum[j] = action->rev_spec[j].value.number;
              else if (action->rev_spec[j].kind == svn_opt_revision_head)
                {
                  revnum[j] = mtcc->head_revision;
                }
              else
                return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                         "'%s@...': revision specifier "
                                         "must be a number or 'head'",
                                         action->relpath[j]);

              rrpath = svn_relpath_join(base_relpath, action->relpath[j], pool);
              parent_rrpath = svn_relpath_dirname(rrpath, pool);

              path_name[j] = svn_relpath_basename(rrpath, NULL);
              SVN_ERR(find_el_rev_by_rrpath_rev(&el_rev[j], editor,
                                                revnum[j], rrpath,
                                                pool, pool));
              SVN_ERR(find_el_rev_by_rrpath_rev(&parent_el_rev[j], editor,
                                                revnum[j], parent_rrpath,
                                                pool, pool));
            }
        }
      switch (action->action)
        {
        case ACTION_DIFF:
          VERIFY_EID_EXISTS("diff", 0);
          VERIFY_EID_EXISTS("diff", 1);
          {
            SVN_ERR(svn_branch_diff_r(editor,
                                      el_rev[0] /*from*/,
                                      el_rev[1] /*to*/,
                                      svn_branch_diff, "",
                                      iterpool));
          }
          break;
        case ACTION_DIFF_E:
          VERIFY_EID_EXISTS("diff-e", 0);
          VERIFY_EID_EXISTS("diff-e", 1);
          {
            SVN_ERR(svn_branch_diff_r(editor,
                                      el_rev[0] /*from*/,
                                      el_rev[1] /*to*/,
                                      svn_branch_diff_e, "",
                                      iterpool));
          }
          break;
        case ACTION_LOG:
          VERIFY_EID_EXISTS("log", 0);
          VERIFY_EID_EXISTS("log", 1);
          {
            SVN_ERR(svn_branch_log(editor,
                                   el_rev[0] /*from*/,
                                   el_rev[1] /*to*/,
                                   iterpool));
          }
          break;
        case ACTION_LIST_BRANCHES:
          {
            VERIFY_EID_EXISTS("branches", 0);
            SVN_ERR(family_list_branch_instances(
                      el_rev[0]->branch->rev_root,
                      el_rev[0]->branch->sibling_defn->family,
                      FALSE, FALSE, iterpool));
          }
          break;
        case ACTION_LIST_BRANCHES_R:
          {
            SVN_ERR(find_el_rev_by_rrpath_rev(
                      &el_rev[0], editor, SVN_INVALID_REVNUM, base_relpath,
                      pool, pool));

            SVN_ERR(family_list_branch_instances(
                      el_rev[0]->branch->rev_root,
                      el_rev[0]->branch->sibling_defn->family,
                      TRUE, TRUE, iterpool));
          }
          break;
        case ACTION_BRANCH:
          VERIFY_EID_EXISTS("branch", 0);
          VERIFY_REV_UNSPECIFIED("branch", 1);
          VERIFY_EID_NONEXISTENT("branch", 1);
          VERIFY_PARENT_EID_EXISTS("branch", 1);
          {
            svn_branch_instance_t *new_branch;

            SVN_ERR(svn_branch_branch(&new_branch,
                                      el_rev[0]->branch, el_rev[0]->eid,
                                      el_rev[1]->branch, parent_el_rev[1]->eid,
                                      path_name[1],
                                      iterpool));
            notify("A+   %s%s", action->relpath[1],
                   branch_str(new_branch, iterpool));
          }
          made_changes = TRUE;
          break;
        case ACTION_MKBRANCH:
          VERIFY_REV_UNSPECIFIED("mkbranch", 0);
          VERIFY_EID_NONEXISTENT("mkbranch", 0);
          VERIFY_PARENT_EID_EXISTS("mkbranch", 0);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_element_content_t *content
              = svn_element_content_create_dir(props, iterpool);
            int new_eid;
            svn_branch_instance_t *new_branch;

            SVN_ERR(svn_editor3_add(editor, &new_eid, svn_node_dir,
                                    parent_el_rev[0]->branch,
                                    parent_el_rev[0]->eid, path_name[0],
                                    content));
            SVN_ERR(svn_branch_branchify(&new_branch,
                                         parent_el_rev[0]->branch, new_eid,
                                         iterpool));
            notify("A    %s%s", action->relpath[0],
                   branch_str(new_branch, iterpool));
          }
          made_changes = TRUE;
          break;
        case ACTION_BRANCHIFY:
          VERIFY_REV_UNSPECIFIED("branchify", 0);
          VERIFY_EID_EXISTS("branchify", 0);
          {
            svn_branch_instance_t *new_branch;

            SVN_ERR(svn_branch_branchify(&new_branch,
                                         el_rev[0]->branch, el_rev[0]->eid,
                                         iterpool));
            notify("R    %s%s", action->relpath[0],
                   branch_str(new_branch, iterpool));
          }
          made_changes = TRUE;
          break;
        case ACTION_DISSOLVE:
          return svn_error_create(SVN_ERR_BRANCHING, NULL,
                                  _("'dissolve' operation not implemented"));
          VERIFY_REV_UNSPECIFIED("dissolve", 0);
          VERIFY_EID_EXISTS("dissolve", 0);
          made_changes = TRUE;
          break;
        case ACTION_MERGE:
          {
            VERIFY_EID_EXISTS("merge", 0);
            VERIFY_EID_EXISTS("merge", 1);
            VERIFY_EID_EXISTS("merge", 2);
            SVN_ERR(svn_branch_merge(editor,
                                     el_rev[0] /*from*/,
                                     el_rev[1] /*to*/,
                                     el_rev[2] /*yca*/,
                                     iterpool));
          }
          made_changes = TRUE;
          break;
        case ACTION_MV:
          if (svn_relpath_skip_ancestor(action->relpath[0], action->relpath[1]))
            return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                     _("mv: cannot move to child of self"));
          VERIFY_REV_UNSPECIFIED("mv", 0);
          VERIFY_EID_EXISTS("mv", 0);
          VERIFY_REV_UNSPECIFIED("mv", 1);
          VERIFY_EID_NONEXISTENT("mv", 1);
          VERIFY_PARENT_EID_EXISTS("mv", 1);
          SVN_ERR(do_move(editor, el_rev[0], parent_el_rev[1], path_name[1],
                          pool));
          notify("V    %s (from %s)", action->relpath[1], action->relpath[0]);
          made_changes = TRUE;
          break;
        case ACTION_CP:
          VERIFY_REV_SPECIFIED("cp", 0);
            /* (Or do we want to support copying from "this txn" too?) */
          VERIFY_EID_EXISTS("cp", 0);
          VERIFY_REV_UNSPECIFIED("cp", 1);
          VERIFY_EID_NONEXISTENT("cp", 1);
          VERIFY_PARENT_EID_EXISTS("cp", 1);
          SVN_ERR(svn_editor3_copy_tree(editor,
                                        el_rev[0],
                                        parent_el_rev[1]->branch,
                                        parent_el_rev[1]->eid, path_name[1]));
          notify("A+   %s (from %s)", action->relpath[1], action->relpath[0]);
          made_changes = TRUE;
          break;
        case ACTION_RM:
          VERIFY_REV_UNSPECIFIED("rm", 0);
          VERIFY_EID_EXISTS("rm", 0);
          {
            svn_branch_instance_t *branch = el_rev[0]->branch;
            int eid = el_rev[0]->eid;

            /* If given a branch root element, look instead at the
               subbranch-root element within the outer branch. */
            if (is_branch_root_element(branch, eid))
              {
                if (! branch->outer_branch)
                  return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                     _("rm: cannot remove the repository root"));

                eid = el_rev[0]->branch->outer_eid;
                branch = el_rev[0]->branch->outer_branch;
              }

            SVN_ERR(svn_editor3_delete(editor, el_rev[0]->rev,
                                       branch, eid));
          }
          notify("D    %s", action->relpath[0]);
          made_changes = TRUE;
          break;
        case ACTION_MKDIR:
          VERIFY_REV_UNSPECIFIED("mkdir", 0);
          VERIFY_EID_NONEXISTENT("mkdir", 0);
          VERIFY_PARENT_EID_EXISTS("mkdir", 0);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_element_content_t *content
              = svn_element_content_create_dir(props, iterpool);
            int new_eid;

            SVN_ERR(svn_editor3_add(editor, &new_eid, svn_node_dir,
                                    parent_el_rev[0]->branch,
                                    parent_el_rev[0]->eid, path_name[0],
                                    content));
          }
          notify("A    %s", action->relpath[0]);
          made_changes = TRUE;
          break;
        case ACTION_PUT_FILE:
          VERIFY_REV_UNSPECIFIED("put", 1);
          VERIFY_PARENT_EID_EXISTS("put", 1);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_stringbuf_t *text;
            svn_element_content_t *content;

            if (el_rev[1]->eid >= 0)
              {
                /* ### get existing props */
                props = apr_hash_make(iterpool);
              }
            else
              {
                props = apr_hash_make(iterpool);
              }
            /* read new text from file */
            {
              svn_stream_t *src;

              if (strcmp(action->relpath[0], "-") != 0)
                SVN_ERR(svn_stream_open_readonly(&src, action->relpath[0],
                                                 pool, iterpool));
              else
                SVN_ERR(svn_stream_for_stdin(&src, pool));

              svn_stringbuf_from_stream(&text, src, 0, iterpool);
            }
            content = svn_element_content_create_file(props, text, iterpool);

            if (el_rev[1]->eid >= 0)
              {
                SVN_ERR(svn_editor3_alter(editor, SVN_INVALID_REVNUM,
                                          el_rev[1]->branch, el_rev[1]->eid,
                                          parent_el_rev[1]->eid, path_name[1],
                                          content));
              }
            else
              {
                int new_eid;

                SVN_ERR(svn_editor3_add(editor, &new_eid, svn_node_file,
                                        parent_el_rev[1]->branch,
                                        parent_el_rev[1]->eid, path_name[1],
                                        content));
              }
          }
          notify("A    %s", action->relpath[1]);
          made_changes = TRUE;
          break;
        default:
          SVN_ERR_MALFUNCTION();
        }
    }

  if (made_changes)
    {
      err = mtcc_commit(mtcc, pool);
    }
  else
    {
      err = svn_editor3_abort(mtcc->editor);
    }

  svn_pool_destroy(mtcc->pool);

  svn_pool_destroy(iterpool);
  return svn_error_trace(err);
}

/* Perform the typical suite of manipulations for user-provided URLs
   on URL, returning the result (allocated from POOL): IRI-to-URI
   conversion, auto-escaping, and canonicalization. */
static const char *
sanitize_url(const char *url,
             apr_pool_t *pool)
{
  url = svn_path_uri_from_iri(url, pool);
  url = svn_path_uri_autoescape(url, pool);
  return svn_uri_canonicalize(url, pool);
}

/* Print a usage message on STREAM. */
static void
usage(FILE *stream, apr_pool_t *pool)
{
  svn_error_clear(svn_cmdline_fputs(
    _("usage: svnmover -U REPO_URL [ACTION...]\n"
      "A client for experimenting with move tracking.\n"
      "\n"
      "  Perform URL-based ACTIONs on a Subversion repository, committing the\n"
      "  result as a (single) new revision, similar to svnmucc.\n"
      "\n"
      "  With no ACTIONs, read actions interactively from standard input, making\n"
      "  one commit for each line of input.\n"
      "\n"
      "  Store move tracking metadata either in local files or in revprops.\n"
      "\n"
      "Actions:\n"
      "  branches PATH          : list all branches in the same family as that at PATH\n"
      "  ls-br-r                : list all branches, recursively\n"
      "  log FROM@REV TO@REV    : show per-revision diffs between FROM and TO\n"
      "  branch SRC DST         : branch the branch-root or branch-subtree at SRC\n"
      "                           to make a new branch at DST\n"
      "  mkbranch ROOT          : make a directory that's the root of a new branch\n"
      "                           in a new branching family; like mkdir+branchify\n"
      "  branchify ROOT         : change the existing simple subtree at ROOT into\n"
      "                           a sub-branch (presently, in a new branch family)\n"
      "  dissolve ROOT          : change the existing sub-branch at ROOT into a\n"
      "                           simple sub-tree of its parent branch\n"
      "  diff LEFT RIGHT        : diff LEFT to RIGHT\n"
      "  diff-e LEFT RIGHT      : diff LEFT to RIGHT (element-focused output)\n"
      "  merge FROM TO YCA@REV  : merge changes YCA->FROM and YCA->TO into TO\n"
      "  cp REV SRC DST         : copy SRC@REV to DST\n"
      "  mv SRC DST             : move SRC to DST\n"
      "  rm PATH                : delete PATH\n"
      "  mkdir PATH             : create new directory PATH\n"
      "  put LOCAL_FILE PATH    : add or modify file PATH with text copied from\n"
      "                           LOCAL_FILE (use \"-\" to read from standard input)\n"
      "\n"
      "Valid options:\n"
      "  -h, -? [--help]        : display this text\n"
      "  -v [--verbose]         : display debugging messages\n"
      "  -q [--quiet]           : suppress notifications\n"
      "  -m [--message] ARG     : use ARG as a log message\n"
      "  -F [--file] ARG        : read log message from file ARG\n"
      "  -u [--username] ARG    : commit the changes as username ARG\n"
      "  -p [--password] ARG    : use ARG as the password\n"
      "  -U [--root-url] ARG    : interpret all action URLs relative to ARG\n"
      "  -r [--revision] ARG    : use revision ARG as baseline for changes\n"
      "  --with-revprop ARG     : set revision property in the following format:\n"
      "                               NAME[=VALUE]\n"
      "  --non-interactive      : do no interactive prompting (default is to\n"
      "                           prompt only if standard input is a terminal)\n"
      "  --force-interactive    : do interactive prompting even if standard\n"
      "                           input is not a terminal\n"
      "  --trust-server-cert    : accept SSL server certificates from unknown\n"
      "                           certificate authorities without prompting (but\n"
      "                           only with '--non-interactive')\n"
      "  -X [--extra-args] ARG  : append arguments from file ARG (one per line;\n"
      "                           use \"-\" to read from standard input)\n"
      "  --config-dir ARG       : use ARG to override the config directory\n"
      "  --config-option ARG    : use ARG to override a configuration option\n"
      "  --no-auth-cache        : do not cache authentication tokens\n"
      "  --version              : print version information\n"),
                  stream, pool));
}

static svn_error_t *
insufficient(void)
{
  return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                          "insufficient arguments");
}

static svn_error_t *
display_version(apr_getopt_t *os, apr_pool_t *pool)
{
  const char *ra_desc_start
    = "The following repository access (RA) modules are available:\n\n";
  svn_stringbuf_t *version_footer;

  version_footer = svn_stringbuf_create(ra_desc_start, pool);
  SVN_ERR(svn_ra_print_modules(version_footer, pool));

  SVN_ERR(svn_opt_print_help4(os, "svnmover", TRUE, FALSE, FALSE,
                              version_footer->data,
                              NULL, NULL, NULL, NULL, NULL, pool));

  return SVN_NO_ERROR;
}

/* Return an error about the mutual exclusivity of the -m, -F, and
   --with-revprop=svn:log command-line options. */
static svn_error_t *
mutually_exclusive_logs_error(void)
{
  return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                          _("--message (-m), --file (-F), and "
                            "--with-revprop=svn:log are mutually "
                            "exclusive"));
}

/* Obtain the log message from multiple sources, producing an error
   if there are multiple sources. Store the result in *FINAL_MESSAGE.  */
static svn_error_t *
sanitize_log_sources(const char **final_message,
                     const char *message,
                     apr_hash_t *revprops,
                     svn_stringbuf_t *filedata,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_string_t *msg;

  *final_message = NULL;
  /* If we already have a log message in the revprop hash, then just
     make sure the user didn't try to also use -m or -F.  Otherwise,
     we need to consult -m or -F to find a log message, if any. */
  msg = svn_hash_gets(revprops, SVN_PROP_REVISION_LOG);
  if (msg)
    {
      if (filedata || message)
        return mutually_exclusive_logs_error();

      *final_message = apr_pstrdup(result_pool, msg->data);

      /* Will be re-added by libsvn_client */
      svn_hash_sets(revprops, SVN_PROP_REVISION_LOG, NULL);
    }
  else if (filedata)
    {
      if (message)
        return mutually_exclusive_logs_error();

      *final_message = apr_pstrdup(result_pool, filedata->data);
    }
  else if (message)
    {
      *final_message = apr_pstrdup(result_pool, message);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
log_message_func(const char **log_msg,
                 svn_boolean_t non_interactive,
                 const char *log_message,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  if (log_message)
    {
      svn_string_t *message = svn_string_create(log_message, pool);

      SVN_ERR_W(svn_subst_translate_string2(&message, NULL, NULL,
                                            message, NULL, FALSE,
                                            pool, pool),
                _("Error normalizing log message to internal format"));

      *log_msg = message->data;

      return SVN_NO_ERROR;
    }

  if (non_interactive)
    {
      return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
                              _("Cannot invoke editor to get log message "
                                "when non-interactive"));
    }
  else
    {
      svn_string_t *msg = svn_string_create("", pool);

      SVN_ERR(svn_cmdline__edit_string_externally(
                      &msg, NULL, NULL, "", msg, "svnmover-commit",
                      ctx->config, TRUE, NULL, pool));

      if (msg && msg->data)
        *log_msg = msg->data;
      else
        *log_msg = NULL;

      return SVN_NO_ERROR;
    }
}

/* Parse the action arguments into action structures. */
static svn_error_t *
parse_actions(apr_array_header_t **actions,
              apr_array_header_t *action_args,
              apr_pool_t *pool)
{
  int i;

  *actions = apr_array_make(pool, 1, sizeof(action_t *));

  for (i = 0; i < action_args->nelts; ++i)
    {
      int j, num_url_args;
      const char *action_string = APR_ARRAY_IDX(action_args, i, const char *);
      action_t *action = apr_pcalloc(pool, sizeof(*action));
      const char *cp_from_rev = NULL;

      /* First, parse the action. */
      if (! strcmp(action_string, "?") || ! strcmp(action_string, "h")
          || ! strcmp(action_string, "help"))
        {
          usage(stdout, pool);
          return SVN_NO_ERROR;
        }
      for (j = 0; j < sizeof(action_defn) / sizeof(action_defn[0]); j++)
        {
          if (strcmp(action_string, action_defn[j].name) == 0)
            {
              action->action = action_defn[j].code;
              num_url_args = action_defn[j].num_args;
              break;
            }
        }
      if (j == sizeof(action_defn) / sizeof(action_defn[0]))
        return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                 "'%s' is not an action",
                                 action_string);

      if (action->action == ACTION_CP)
        {
          /* next argument is the copy source revision */
          if (++i == action_args->nelts)
            return svn_error_trace(insufficient());
          cp_from_rev = APR_ARRAY_IDX(action_args, i, const char *);
        }

      /* Parse the required number of URLs. */
      for (j = 0; j < num_url_args; ++j)
        {
          const char *path;

          if (++i == action_args->nelts)
            return svn_error_trace(insufficient());
          path = APR_ARRAY_IDX(action_args, i, const char *);

          if (cp_from_rev && j == 0)
            {
              path = apr_psprintf(pool, "%s@%s", path, cp_from_rev);
            }

          SVN_ERR(svn_opt_parse_path(&action->rev_spec[j], &path, path, pool));

          /* If there's an ANCHOR_URL, we expect URL to be a path
             relative to ANCHOR_URL (and we build a full url from the
             combination of the two).  Otherwise, it should be a full
             url. */
          if (svn_path_is_url(path))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is a URL; use "
                                       "--root-url (-U) instead", path);
            }
          if (! svn_relpath_is_canonical(path))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is not a relative path "
                                       "or a URL", path);
            }
          action->relpath[j] = path;
        }

      APR_ARRAY_PUSH(*actions, action_t *) = action;
    }

  return SVN_NO_ERROR;
}

/*
 * On success, leave *EXIT_CODE untouched and return SVN_NO_ERROR. On error,
 * either return an error to be displayed, or set *EXIT_CODE to non-zero and
 * return SVN_NO_ERROR.
 */
static svn_error_t *
sub_main(int *exit_code, int argc, const char *argv[], apr_pool_t *pool)
{
  apr_array_header_t *actions;
  svn_error_t *err = SVN_NO_ERROR;
  apr_getopt_t *opts;
  enum {
    config_dir_opt = SVN_OPT_FIRST_LONGOPT_ID,
    config_inline_opt,
    no_auth_cache_opt,
    version_opt,
    with_revprop_opt,
    non_interactive_opt,
    force_interactive_opt,
    trust_server_cert_opt
  };
  static const apr_getopt_option_t options[] = {
    {"verbose", 'v', 0, ""},
    {"quiet", 'q', 0, ""},
    {"branch", 'b', 1, ""},
    {"message", 'm', 1, ""},
    {"file", 'F', 1, ""},
    {"username", 'u', 1, ""},
    {"password", 'p', 1, ""},
    {"root-url", 'U', 1, ""},
    {"revision", 'r', 1, ""},
    {"with-revprop",  with_revprop_opt, 1, ""},
    {"extra-args", 'X', 1, ""},
    {"help", 'h', 0, ""},
    {NULL, '?', 0, ""},
    {"non-interactive", non_interactive_opt, 0, ""},
    {"force-interactive", force_interactive_opt, 0, ""},
    {"trust-server-cert", trust_server_cert_opt, 0, ""},
    {"config-dir", config_dir_opt, 1, ""},
    {"config-option",  config_inline_opt, 1, ""},
    {"no-auth-cache",  no_auth_cache_opt, 0, ""},
    {"version", version_opt, 0, ""},
    {NULL, 0, 0, NULL}
  };
  const char *message = "";
  svn_stringbuf_t *filedata = NULL;
  const char *username = NULL, *password = NULL;
  const char *anchor_url = NULL, *extra_args_file = NULL;
  const char *config_dir = NULL;
  apr_array_header_t *config_options;
  svn_boolean_t non_interactive = FALSE;
  svn_boolean_t force_interactive = FALSE;
  svn_boolean_t interactive_actions;
  svn_boolean_t trust_server_cert = FALSE;
  svn_boolean_t no_auth_cache = FALSE;
  svn_revnum_t base_revision = SVN_INVALID_REVNUM;
  apr_array_header_t *action_args;
  apr_hash_t *revprops = apr_hash_make(pool);
  apr_hash_t *cfg_hash;
  svn_config_t *cfg_config;
  svn_client_ctx_t *ctx;
  const char *log_msg;

  /* Check library versions */
  SVN_ERR(check_lib_versions());

  /* Suppress debug message unless '-v' given. */
  svn_dbg__set_quiet_mode(TRUE);

  config_options = apr_array_make(pool, 0,
                                  sizeof(svn_cmdline__config_argument_t*));

  apr_getopt_init(&opts, pool, argc, argv);
  opts->interleave = 1;
  while (1)
    {
      int opt;
      const char *arg;
      const char *opt_arg;

      apr_status_t status = apr_getopt_long(opts, options, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        return svn_error_wrap_apr(status, "getopt failure");
      switch(opt)
        {
        case 'v':
          svn_dbg__set_quiet_mode(FALSE);
          break;
        case 'q':
          quiet = TRUE;
          break;
        case 'm':
          SVN_ERR(svn_utf_cstring_to_utf8(&message, arg, pool));
          break;
        case 'F':
          {
            const char *arg_utf8;
            SVN_ERR(svn_utf_cstring_to_utf8(&arg_utf8, arg, pool));
            SVN_ERR(svn_stringbuf_from_file2(&filedata, arg, pool));
          }
          break;
        case 'u':
          username = apr_pstrdup(pool, arg);
          break;
        case 'p':
          password = apr_pstrdup(pool, arg);
          break;
        case 'U':
          SVN_ERR(svn_utf_cstring_to_utf8(&anchor_url, arg, pool));
          if (! svn_path_is_url(anchor_url))
            return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                     "'%s' is not a URL", anchor_url);
          anchor_url = sanitize_url(anchor_url, pool);
          break;
        case 'r':
          {
            const char *saved_arg = arg;
            char *digits_end = NULL;
            while (*arg == 'r')
              arg++;
            base_revision = strtol(arg, &digits_end, 10);
            if ((! SVN_IS_VALID_REVNUM(base_revision))
                || (! digits_end)
                || *digits_end)
              return svn_error_createf(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                       _("Invalid revision number '%s'"),
                                       saved_arg);
          }
          break;
        case with_revprop_opt:
          SVN_ERR(svn_opt_parse_revprop(&revprops, arg, pool));
          break;
        case 'X':
          extra_args_file = apr_pstrdup(pool, arg);
          break;
        case non_interactive_opt:
          non_interactive = TRUE;
          break;
        case force_interactive_opt:
          force_interactive = TRUE;
          break;
        case trust_server_cert_opt:
          trust_server_cert = TRUE;
          break;
        case config_dir_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&config_dir, arg, pool));
          break;
        case config_inline_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_cmdline__parse_config_option(config_options, opt_arg,
                                                   pool));
          break;
        case no_auth_cache_opt:
          no_auth_cache = TRUE;
          break;
        case version_opt:
          SVN_ERR(display_version(opts, pool));
          return SVN_NO_ERROR;
        case 'h':
        case '?':
          usage(stdout, pool);
          return SVN_NO_ERROR;
        }
    }

  if (non_interactive && force_interactive)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("--non-interactive and --force-interactive "
                                "are mutually exclusive"));
    }
  else
    non_interactive = !svn_cmdline__be_interactive(non_interactive,
                                                   force_interactive);

  if (trust_server_cert && !non_interactive)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("--trust-server-cert requires "
                                "--non-interactive"));
    }

  /* Now initialize the client context */

  err = svn_config_get_config(&cfg_hash, config_dir, pool);
  if (err)
    {
      /* Fallback to default config if the config directory isn't readable
         or is not a directory. */
      if (APR_STATUS_IS_EACCES(err->apr_err)
          || SVN__APR_STATUS_IS_ENOTDIR(err->apr_err))
        {
          svn_handle_warning2(stderr, err, "svnmover: ");
          svn_error_clear(err);

          SVN_ERR(svn_config__get_default_config(&cfg_hash, pool));
        }
      else
        return err;
    }

  if (config_options)
    {
      svn_error_clear(
          svn_cmdline__apply_config_options(cfg_hash, config_options,
                                            "svnmover: ", "--config-option"));
    }

  SVN_ERR(svn_client_create_context2(&ctx, cfg_hash, pool));

  cfg_config = svn_hash_gets(cfg_hash, SVN_CONFIG_CATEGORY_CONFIG);
  SVN_ERR(svn_cmdline_create_auth_baton(&ctx->auth_baton,
                                        non_interactive,
                                        username,
                                        password,
                                        config_dir,
                                        no_auth_cache,
                                        trust_server_cert,
                                        cfg_config,
                                        ctx->cancel_func,
                                        ctx->cancel_baton,
                                        pool));

  /* Make sure we have a log message to use. */
  SVN_ERR(sanitize_log_sources(&log_msg, message, revprops, filedata,
                               pool, pool));

  /* Get the commit log message */
  SVN_ERR(log_message_func(&log_msg, non_interactive, log_msg, ctx, pool));
  if (! log_msg)
    return SVN_NO_ERROR;

  /* Put the log message in the list of revprops, and check that the user
     did not try to supply any other "svn:*" revprops. */
  if (svn_prop_has_svn_prop(revprops, pool))
    return svn_error_create(SVN_ERR_CLIENT_PROPERTY_NAME, NULL,
                            _("Standard properties can't be set "
                              "explicitly as revision properties"));
  svn_hash_sets(revprops, SVN_PROP_REVISION_LOG,
                svn_string_create(log_msg, pool));

  if (!anchor_url)
    return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                             "--root-url (-U) not provided");

  /* Copy the rest of our command-line arguments to an array,
     UTF-8-ing them along the way. */
  /* If there are extra arguments in a supplementary file, tack those
     on, too (again, in UTF8 form). */
  action_args = apr_array_make(pool, opts->argc, sizeof(const char *));
  if (extra_args_file)
    {
      const char *extra_args_file_utf8;
      svn_stringbuf_t *contents, *contents_utf8;

      SVN_ERR(svn_utf_cstring_to_utf8(&extra_args_file_utf8,
                                      extra_args_file, pool));
      SVN_ERR(svn_stringbuf_from_file2(&contents, extra_args_file_utf8, pool));
      SVN_ERR(svn_utf_stringbuf_to_utf8(&contents_utf8, contents, pool));
      svn_cstring_split_append(action_args, contents_utf8->data, "\n\r",
                               FALSE, pool);
    }

  interactive_actions = !(opts->ind < opts->argc
                          || extra_args_file
                          || non_interactive);

  do
    {
      /* Parse arguments -- converting local style to internal style,
       * repos-relative URLs to regular URLs, etc. */
      err = svn_client_args_to_target_array2(&action_args, opts, action_args,
                                             ctx, FALSE, pool);
      if (! err)
        err = parse_actions(&actions, action_args, pool);
      if (! err)
        err = execute(actions, anchor_url, revprops, base_revision, ctx, pool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_AUTHN_FAILED && non_interactive)
            err = svn_error_quick_wrap(err,
                                       _("Authentication failed and interactive"
                                         " prompting is disabled; see the"
                                         " --force-interactive option"));
          if (interactive_actions)
            {
              svn_handle_warning2(stderr, err, "svnmover: ");
              svn_error_clear(err);
            }
          else
            SVN_ERR(err);
        }

      /* Possibly read more actions from the command line */
      if (interactive_actions)
        {
          const char *input;

          SVN_ERR(svn_cmdline_prompt_user2(&input, "svnmover> ", NULL, pool));
          action_args = svn_cstring_split(input, " ",
                                          TRUE /*chop_whitespace*/, pool);
        }
    }
  while (interactive_actions);

  return SVN_NO_ERROR;
}

int
main(int argc, const char *argv[])
{
  apr_pool_t *pool;
  int exit_code = EXIT_SUCCESS;
  svn_error_t *err;

  /* Initialize the app. */
  if (svn_cmdline_init("svnmover", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create our top-level pool.  Use a separate mutexless allocator,
   * given this application is single threaded.
   */
  pool = apr_allocator_owner_get(svn_pool_create_allocator(FALSE));

  svn_error_set_malfunction_handler(svn_error_raise_on_malfunction);

  err = sub_main(&exit_code, argc, argv, pool);

  /* Flush stdout and report if it fails. It would be flushed on exit anyway
     but this makes sure that output is not silently lost if it fails. */
  err = svn_error_compose_create(err, svn_cmdline_fflush(stdout));

  if (err)
    {
      exit_code = EXIT_FAILURE;
      svn_cmdline_handle_exit_error(err, NULL, "svnmover: ");
    }

  svn_pool_destroy(pool);
  return exit_code;
}
