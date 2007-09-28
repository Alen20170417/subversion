/* node-rev.c --- storing and retrieving NODE-REVISION skels
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <string.h>

#define APU_WANT_DB
#include <apu_want.h>

#include "svn_pools.h"
#include "svn_fs.h"
#include "fs.h"
#include "id.h"
#include "err.h"
#include "node-rev.h"
#include "reps-strings.h"
#include "revs-txns.h"

#include "bdb/nodes-table.h"
#include "bdb/successors-table.h"


/* Creating completely new nodes.  */


svn_error_t *
svn_fs_base__create_node(const svn_fs_id_t **id_p,
                         svn_fs_t *fs,
                         node_revision_t *noderev,
                         const char *copy_id,
                         const char *txn_id,
                         trail_t *trail,
                         apr_pool_t *pool)
{
  svn_fs_id_t *id;

  /* Find an unused ID for the node.  */
  SVN_ERR(svn_fs_bdb__new_node_id(&id, fs, copy_id, txn_id, trail, pool));

  /* Store its NODE-REVISION skel.  */
  SVN_ERR(svn_fs_bdb__put_node_revision(fs, id, noderev, trail, pool));

  *id_p = id;
  return SVN_NO_ERROR;
}



/* Creating new revisions of existing nodes.  */

svn_error_t *
svn_fs_base__create_successor(const svn_fs_id_t **new_id_p,
                              svn_fs_t *fs,
                              const svn_fs_id_t *old_id,
                              node_revision_t *new_noderev,
                              const char *copy_id,
                              const char *txn_id,
                              trail_t *trail,
                              apr_pool_t *pool)
{
  svn_fs_id_t *new_id;
  svn_string_t *new_id_str, *old_id_str;

  /* Choose an ID for the new node, and store it in the database.  */
  SVN_ERR(svn_fs_bdb__new_successor_id(&new_id, fs, old_id, copy_id,
                                       txn_id, trail, pool));

  /* Store the new skel under that ID.  */
  SVN_ERR(svn_fs_bdb__put_node_revision(fs, new_id, new_noderev,
                                        trail, pool));

  /* Record the successor relationship. */
  old_id_str = svn_fs_unparse_id(old_id, pool);
  new_id_str = svn_fs_unparse_id(new_id, pool);
  SVN_ERR(svn_fs_bdb__successors_add(fs, old_id_str->data, new_id_str->data,
                                     trail, pool));

  *new_id_p = new_id;
  return SVN_NO_ERROR;
}



/* Deleting a node revision. */

svn_error_t *
svn_fs_base__delete_node_revision(svn_fs_t *fs,
                                  const svn_fs_id_t *id,
                                  const svn_fs_id_t *pred_id,
                                  trail_t *trail,
                                  apr_pool_t *pool)
{
  svn_string_t *node_id_str, *succ_id_str;

  /* ### TODO: here, we should adjust other nodes to compensate for
     the missing node. */

  /* Remove the successor association... */
  if (pred_id)
    {
      node_id_str = svn_fs_unparse_id(pred_id, pool);
      succ_id_str = svn_fs_unparse_id(id, pool);
      SVN_ERR(svn_fs_bdb__successors_delete(fs, node_id_str->data, 
                                            succ_id_str->data, trail, 
                                            pool));
    }

  /* ...and then the node itself. */
  return svn_fs_bdb__delete_nodes_entry(fs, id, trail, pool);
}



/* Fetching node successors. */

svn_error_t *
svn_fs_base__get_node_successors(apr_array_header_t **successors_p,
                                 svn_fs_t *fs,
                                 const svn_fs_id_t *id,
                                 svn_boolean_t committed_only,
                                 trail_t *trail,
                                 apr_pool_t *pool)
{
  apr_array_header_t *all_successors, *successors;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_string_t *node_id_str = svn_fs_unparse_id(id, pool);
  int i;

  SVN_ERR(svn_fs_bdb__successors_fetch(&all_successors, fs, node_id_str->data,
                                       trail, pool));
  successors = apr_array_make(pool, all_successors->nelts, 
                              sizeof(const svn_fs_id_t *));
  for (i = 0; i < all_successors->nelts; i++)
    {
      svn_revnum_t revision;
      const char *succ_id_str = APR_ARRAY_IDX(all_successors, i, const char *);
      const svn_fs_id_t *succ_id = svn_fs_parse_id(succ_id_str, 
                                                   strlen(succ_id_str), pool);

      svn_pool_clear(subpool);

      /* If we only want stable, committed successor IDs, then we need
         to check each ID's txn-id component to verify that's been
         committed. */
      if (committed_only)
        {
          SVN_ERR(svn_fs_base__txn_get_revision
                  (&revision, fs, svn_fs_base__id_txn_id(succ_id), 
                   trail, subpool));
          if (! SVN_IS_VALID_REVNUM(revision))
            continue;
        }
            
      APR_ARRAY_PUSH(successors, const svn_fs_id_t *) = succ_id;
    }
  svn_pool_destroy(subpool);

  *successors_p = successors;
  return SVN_NO_ERROR;
}
