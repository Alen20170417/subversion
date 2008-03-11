/*
 * cache-test.c -- test the in-memory cache
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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

#include <stdio.h>
#include <string.h>
#include <apr_general.h>

#include "svn_cache.h"
#include "svn_pools.h"

#include "../svn_test.h"

static svn_cache_dup_func_t dup_revnum;
static svn_error_t *
dup_revnum(void **out,
           void *in,
           apr_pool_t *pool)
{
  svn_revnum_t *in_rn = in, *duped = apr_palloc(pool, sizeof(*duped));

  *duped = *in_rn;

  *out = duped;

  return SVN_NO_ERROR;
}

static svn_error_t *
test_cache_basic(const char **msg,
                 svn_boolean_t msg_only,
                 svn_test_opts_t *opts,
                 apr_pool_t *pool)
{
  svn_cache_t *cache;
  svn_boolean_t found;
  svn_revnum_t twenty = 20, thirty = 30, *answer;
  apr_pool_t *subpool;

  *msg = "basic svn_cache test";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a cache with just one entry. */
  SVN_ERR(svn_cache_create(&cache,
                           dup_revnum,
                           APR_HASH_KEY_STRING,
                           1,
                           1,
                           TRUE,
                           pool));

  /* We use a subpool for all calls in this test and aggressively
   * clear it, to try to find any bugs where the cached values aren't
   * actually saved away in the cache's pools. */
  subpool = svn_pool_create(pool);

  SVN_ERR(svn_cache_get((void **) &answer, &found, cache, "twenty", subpool));
  if (found)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "cache found an entry that wasn't there");
  svn_pool_clear(subpool);

  SVN_ERR(svn_cache_set(cache, "twenty", &twenty, subpool));
  svn_pool_clear(subpool);

  SVN_ERR(svn_cache_get((void **) &answer, &found, cache, "twenty", subpool));
  if (! found)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "cache failed to find entry for 'twenty'");
  if (*answer != 20)
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                             "expected 20 but found '%ld'", *answer);
  svn_pool_clear(subpool);

  SVN_ERR(svn_cache_set(cache, "thirty", &thirty, subpool));
  svn_pool_clear(subpool);

  SVN_ERR(svn_cache_get((void **) &answer, &found, cache, "thirty", subpool));
  if (! found)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "cache failed to find entry for 'thirty'");
  if (*answer != 30)
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                             "expected 30 but found '%ld'", *answer);

  SVN_ERR(svn_cache_get((void **) &answer, &found, cache, "twenty", subpool));
  if (found)
    return svn_error_create(SVN_ERR_TEST_FAILED, NULL,
                            "cache found entry for 'twenty' that should have "
                            "expired");
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


/* The test table.  */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS(test_cache_basic),
    SVN_TEST_NULL
  };
