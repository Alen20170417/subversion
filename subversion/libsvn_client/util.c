/*
 * util.c :  utility functions for the libsvn_client library
 *
 * ====================================================================
 * Copyright (c) 2005 CollabNet.  All rights reserved.
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

#include <assert.h>
#include <apr_pools.h>
#include <apr_strings.h>

#include "svn_string.h"
#include "svn_error.h"
#include "svn_props.h"
#include "svn_path.h"
#include "svn_client.h"
#include "client.h"

#include "svn_private_config.h"

/**
 * Duplicate a HASH containing (char * -> svn_string_t *) key/value
 * pairs using POOL.
 */
static apr_hash_t *
svn_client__string_hash_dup(apr_hash_t *hash, apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const void *key;
  apr_ssize_t klen;
  void *val;
  apr_hash_t *new_hash = apr_hash_make(pool);
  for (hi = apr_hash_first(pool, hash); hi; hi = apr_hash_next(hi))
    {
      apr_hash_this(hi, &key, &klen, &val);
      key = apr_pstrdup(pool, key);
      val = svn_string_dup(val, pool);
      apr_hash_set(new_hash, key, klen, val);
    }
  return new_hash;
}

svn_client_commit_item2_t *
svn_client_commit_item2_dup(const svn_client_commit_item2_t *item,
                            apr_pool_t *pool)
{
  svn_client_commit_item2_t *new_item = apr_palloc(pool, sizeof(*new_item));

  *new_item = *item;

  if (new_item->path)
    new_item->path = apr_pstrdup(pool, new_item->path);

  if (new_item->url)
    new_item->url = apr_pstrdup(pool, new_item->url);

  if (new_item->copyfrom_url)
    new_item->copyfrom_url = apr_pstrdup(pool, new_item->copyfrom_url);

  if (new_item->wcprop_changes)
    new_item->wcprop_changes = svn_prop_array_dup(new_item->wcprop_changes,
                                                  pool);

  return new_item;
}

svn_client_proplist_item_t *
svn_client_proplist_item_dup(const svn_client_proplist_item_t *item,
                             apr_pool_t * pool)
{
  svn_client_proplist_item_t *new_item
    = apr_pcalloc(pool, sizeof(*new_item));

  if (item->node_name)
    new_item->node_name = svn_stringbuf_dup(item->node_name, pool);

  if (item->prop_hash)
    new_item->prop_hash = svn_client__string_hash_dup(item->prop_hash, pool);

  return new_item;
}

svn_error_t *
svn_client__path_relative_to_root(const char **rel_path,
                                  const char *path_or_url,
                                  const char *repos_root,
                                  svn_ra_session_t *ra_session,
                                  svn_wc_adm_access_t *adm_access,
                                  apr_pool_t *pool)
{
  svn_error_t *err = SVN_NO_ERROR;
  svn_boolean_t need_cleanup = FALSE;
  svn_boolean_t is_path = !svn_path_is_url(path_or_url);

  /* Old WCs may not provide the repository URL. */
  assert(repos_root != NULL || ra_session != NULL);

  /* If we have a WC path, transform it into a URL for use in
     calculating its path relative to the repository root.

     If we don't already know the repository root, derive it by first
     looking in the entries file, then falling back to asking the
     repository itself. */
  if (is_path || repos_root == NULL)
    {
      const svn_wc_entry_t *entry;

      if (adm_access == NULL)
        {
          SVN_ERR(svn_wc_adm_open3(&adm_access, NULL, path_or_url, FALSE, 0,
                                   NULL, NULL, pool));
          need_cleanup = TRUE;
        }
      svn_wc_entry(&entry, path_or_url, adm_access, FALSE, pool);

      if (is_path)
        {
          if (entry != NULL)
            path_or_url = entry->url;
          else
            {
              /* We can't transform the local path into a URL. */
              err = svn_error_createf(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                                      _("'%s' is not under version control"),
                                      svn_path_local_style(path_or_url, pool));
              goto cleanup;
            }
        }

      if (repos_root == NULL)
        {
          if (entry != NULL)
            repos_root = entry->repos;

          if (repos_root == NULL)
            {
              err = svn_ra_get_repos_root(ra_session, &repos_root, pool);
              if (err)
                goto cleanup;
            }
        }
    }

  /* Calculate the path relative to the repository root. */
  *rel_path = svn_path_is_child(repos_root, path_or_url, pool);

  /* Assure that the path begins with a slash, as the path is NULL if
     the URL is the repository root. */
  *rel_path = svn_path_join("/", *rel_path ? *rel_path : "", pool);
  *rel_path = svn_path_uri_decode(*rel_path, pool);

 cleanup:
  if (need_cleanup)
    {
      if (err == SVN_NO_ERROR)
        err = svn_wc_adm_close(adm_access);
      else
        svn_wc_adm_close(adm_access);
    }
  return err;
}
