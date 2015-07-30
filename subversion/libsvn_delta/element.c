/*
 * element.c :  editing trees of versioned resources
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

#include <assert.h>
#include <apr_pools.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_string.h"
#include "svn_props.h"
#include "svn_dirent_uri.h"

#include "private/svn_element.h"
#include "svn_private_config.h"


/*
 * ===================================================================
 * Minor data types
 * ===================================================================
 */

svn_pathrev_t
svn_pathrev_dup(svn_pathrev_t p,
                apr_pool_t *result_pool)
{
  /* The object P is passed by value so we can modify it in place */
  p.relpath = apr_pstrdup(result_pool, p.relpath);
  return p;
}

svn_boolean_t
svn_pathrev_equal(svn_pathrev_t *peg_path1,
                  svn_pathrev_t *peg_path2)
{
  if (peg_path1->rev != peg_path2->rev)
    return FALSE;
  if (strcmp(peg_path1->relpath, peg_path2->relpath) != 0)
    return FALSE;

  return TRUE;
}


/*
 * ===================================================================
 * Element payload
 * ===================================================================
 */

svn_boolean_t
svn_element_payload_invariants(const svn_element_payload_t *payload)
{
  /* If kind is unknown, it's a reference; otherwise it has content
     specified and may also have a reference. */
  if (payload->kind == svn_node_unknown)
    if ((payload->ref.relpath
         && svn_relpath_is_canonical(payload->ref.relpath)
         && SVN_IS_VALID_REVNUM(payload->ref.rev)))
      return TRUE;
  if ((payload->kind == svn_node_dir
       || payload->kind == svn_node_file
       || payload->kind == svn_node_symlink)
      && (payload->props
          && (!payload->text == (payload->kind != svn_node_file))
          && (!payload->target == (payload->kind != svn_node_symlink))))
    return TRUE;
  return FALSE;
}

svn_element_payload_t *
svn_element_payload_dup(const svn_element_payload_t *old,
                        apr_pool_t *result_pool)
{
  svn_element_payload_t *new_payload;

  assert(! old || svn_element_payload_invariants(old));

  if (old == NULL)
    return NULL;

  new_payload = apr_pmemdup(result_pool, old, sizeof(*new_payload));
  if (old->ref.relpath)
    new_payload->ref = svn_pathrev_dup(old->ref, result_pool);
  if (old->props)
    new_payload->props = svn_prop_hash_dup(old->props, result_pool);
  if (old->kind == svn_node_file && old->text)
    new_payload->text = svn_stringbuf_dup(old->text, result_pool);
  if (old->kind == svn_node_symlink && old->target)
    new_payload->target = apr_pstrdup(result_pool, old->target);
  return new_payload;
}

svn_boolean_t
svn_element_payload_equal(const svn_element_payload_t *left,
                          const svn_element_payload_t *right,
                          apr_pool_t *scratch_pool)
{
  apr_array_header_t *prop_diffs;

  assert(! left || svn_element_payload_invariants(left));
  assert(! right || svn_element_payload_invariants(right));

  if (!left && !right)
    {
      return TRUE;
    }
  else if (!left || !right)
    {
      return FALSE;
    }

  /* content defined only by reference is not supported */
  SVN_ERR_ASSERT_NO_RETURN(left->kind != svn_node_unknown
                           && right->kind != svn_node_unknown);

  if (left->kind != right->kind)
    {
      return FALSE;
    }

  svn_error_clear(svn_prop_diffs(&prop_diffs,
                                 left->props, right->props,
                                 scratch_pool));

  if (prop_diffs->nelts != 0)
    {
      return FALSE;
    }
  switch (left->kind)
    {
    case svn_node_dir:
      break;
    case svn_node_file:
      if (! svn_stringbuf_compare(left->text, right->text))
        {
          return FALSE;
        }
      break;
    case svn_node_symlink:
      if (strcmp(left->target, right->target) != 0)
        {
          return FALSE;
        }
      break;
    default:
      break;
    }

  return TRUE;
}

svn_element_payload_t *
svn_element_payload_create_ref(svn_pathrev_t ref,
                               apr_pool_t *result_pool)
{
  svn_element_payload_t *new_payload
    = apr_pcalloc(result_pool, sizeof(*new_payload));

  new_payload->pool = result_pool;
  new_payload->kind = svn_node_unknown;
  new_payload->ref = svn_pathrev_dup(ref, result_pool);
  assert(svn_element_payload_invariants(new_payload));
  return new_payload;
}

svn_element_payload_t *
svn_element_payload_create_dir(apr_hash_t *props,
                               apr_pool_t *result_pool)
{
  svn_element_payload_t *new_payload
    = apr_pcalloc(result_pool, sizeof(*new_payload));

  new_payload->pool = result_pool;
  new_payload->kind = svn_node_dir;
  new_payload->props = props ? svn_prop_hash_dup(props, result_pool) : NULL;
  assert(svn_element_payload_invariants(new_payload));
  return new_payload;
}

svn_element_payload_t *
svn_element_payload_create_file(apr_hash_t *props,
                                svn_stringbuf_t *text,
                                apr_pool_t *result_pool)
{
  svn_element_payload_t *new_payload
    = apr_pcalloc(result_pool, sizeof(*new_payload));

  SVN_ERR_ASSERT_NO_RETURN(text);

  new_payload->pool = result_pool;
  new_payload->kind = svn_node_file;
  new_payload->props = props ? svn_prop_hash_dup(props, result_pool) : NULL;
  new_payload->text = svn_stringbuf_dup(text, result_pool);
  assert(svn_element_payload_invariants(new_payload));
  return new_payload;
}

svn_element_payload_t *
svn_element_payload_create_symlink(apr_hash_t *props,
                                   const char *target,
                                   apr_pool_t *result_pool)
{
  svn_element_payload_t *new_payload
    = apr_pcalloc(result_pool, sizeof(*new_payload));

  SVN_ERR_ASSERT_NO_RETURN(target);

  new_payload->pool = result_pool;
  new_payload->kind = svn_node_symlink;
  new_payload->props = props ? svn_prop_hash_dup(props, result_pool) : NULL;
  new_payload->target = apr_pstrdup(result_pool, target);
  assert(svn_element_payload_invariants(new_payload));
  return new_payload;
}

