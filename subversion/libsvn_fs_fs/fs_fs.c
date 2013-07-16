/* fs_fs.c --- filesystem operations specific to fs_fs
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_uuid.h>
#include <apr_lib.h>
#include <apr_md5.h>
#include <apr_sha1.h>
#include <apr_strings.h>
#include <apr_thread_mutex.h>

#include "svn_pools.h"
#include "svn_fs.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_props.h"
#include "svn_sorts.h"
#include "svn_string.h"
#include "svn_time.h"
#include "svn_mergeinfo.h"
#include "svn_config.h"
#include "svn_ctype.h"
#include "svn_version.h"

#include "cached_data.h"
#include "fs.h"
#include "tree.h"
#include "lock.h"
#include "key-gen.h"
#include "fs_fs.h"
#include "id.h"
#include "low_level.h"
#include "pack.h"
#include "recovery.h"
#include "rep-cache.h"
#include "revprops.h"
#include "temp_serializer.h"
#include "util.h"

#include "private/svn_string_private.h"
#include "private/svn_fs_util.h"
#include "private/svn_subr_private.h"
#include "private/svn_delta_private.h"
#include "../libsvn_fs/fs-loader.h"

#include "svn_private_config.h"
#include "temp_serializer.h"

/* The default maximum number of files per directory to store in the
   rev and revprops directory.  The number below is somewhat arbitrary,
   and can be overridden by defining the macro while compiling; the
   figure of 1000 is reasonable for VFAT filesystems, which are by far
   the worst performers in this area. */
#ifndef SVN_FS_FS_DEFAULT_MAX_FILES_PER_DIR
#define SVN_FS_FS_DEFAULT_MAX_FILES_PER_DIR 1000
#endif

/* Begin deltification after a node history exceeded this this limit.
   Useful values are 4 to 64 with 16 being a good compromise between
   computational overhead and repository size savings.
   Should be a power of 2.
   Values < 2 will result in standard skip-delta behavior. */
#define SVN_FS_FS_MAX_LINEAR_DELTIFICATION 16

/* Finding a deltification base takes operations proportional to the
   number of changes being skipped. To prevent exploding runtime
   during commits, limit the deltification range to this value.
   Should be a power of 2 minus one.
   Values < 1 disable deltification. */
#define SVN_FS_FS_MAX_DELTIFICATION_WALK 1023

/* Notes:

To avoid opening and closing the rev-files all the time, it would
probably be advantageous to keep each rev-file open for the
lifetime of the transaction object.  I'll leave that as a later
optimization for now.

I didn't keep track of pool lifetimes at all in this code.  There
are likely some errors because of that.

*/

/* The vtable associated with an open transaction object. */
static txn_vtable_t txn_vtable = {
  svn_fs_fs__commit_txn,
  svn_fs_fs__abort_txn,
  svn_fs_fs__txn_prop,
  svn_fs_fs__txn_proplist,
  svn_fs_fs__change_txn_prop,
  svn_fs_fs__txn_root,
  svn_fs_fs__change_txn_props
};

/* Declarations. */

static svn_error_t *
get_youngest(svn_revnum_t *youngest_p, const char *fs_path, apr_pool_t *pool);

/* Pathname helper functions */

static const char *
path_format(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_FORMAT, pool);
}

static APR_INLINE const char *
path_uuid(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_UUID, pool);
}

const char *
svn_fs_fs__path_current(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_CURRENT, pool);
}

static APR_INLINE const char *
path_txn_current(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_TXN_CURRENT, pool);
}

static APR_INLINE const char *
path_txn_current_lock(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_TXN_CURRENT_LOCK, pool);
}

static APR_INLINE const char *
path_lock(svn_fs_t *fs, apr_pool_t *pool)
{
  return svn_dirent_join(fs->path, PATH_LOCK_FILE, pool);
}

/* Return the name of the sha1->rep mapping file in transaction TXN_ID
 * within FS for the given SHA1 checksum.  Use POOL for allocations.
 */
static APR_INLINE const char *
path_txn_sha1(svn_fs_t *fs, const char *txn_id, svn_checksum_t *sha1,
              apr_pool_t *pool)
{
  return svn_dirent_join(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                         svn_checksum_to_cstring(sha1, pool),
                         pool);
}

static APR_INLINE const char *
path_txn_changes(svn_fs_t *fs, const char *txn_id, apr_pool_t *pool)
{
  return svn_dirent_join(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                         PATH_CHANGES, pool);
}

static APR_INLINE const char *
path_txn_props(svn_fs_t *fs, const char *txn_id, apr_pool_t *pool)
{
  return svn_dirent_join(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                         PATH_TXN_PROPS, pool);
}

static APR_INLINE const char *
path_txn_next_ids(svn_fs_t *fs, const char *txn_id, apr_pool_t *pool)
{
  return svn_dirent_join(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                         PATH_NEXT_IDS, pool);
}

static APR_INLINE const char *
path_txn_proto_rev_lock(svn_fs_t *fs, const char *txn_id, apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  if (ffd->format >= SVN_FS_FS__MIN_PROTOREVS_DIR_FORMAT)
    return svn_dirent_join_many(pool, fs->path, PATH_TXN_PROTOS_DIR,
                                apr_pstrcat(pool, txn_id, PATH_EXT_REV_LOCK,
                                            (char *)NULL),
                                NULL);
  else
    return svn_dirent_join(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                           PATH_REV_LOCK, pool);
}

static APR_INLINE const char *
path_node_origin(svn_fs_t *fs, const char *node_id, apr_pool_t *pool)
{
  size_t len = strlen(node_id);
  const char *node_id_minus_last_char =
    (len == 1) ? "0" : apr_pstrmemdup(pool, node_id, len - 1);
  return svn_dirent_join_many(pool, fs->path, PATH_NODE_ORIGINS_DIR,
                              node_id_minus_last_char, NULL);
}

static APR_INLINE const char *
path_and_offset_of(apr_file_t *file, apr_pool_t *pool)
{
  const char *path;
  apr_off_t offset = 0;

  if (apr_file_name_get(&path, file) != APR_SUCCESS)
    path = "(unknown)";

  if (apr_file_seek(file, APR_CUR, &offset) != APR_SUCCESS)
    offset = -1;

  return apr_psprintf(pool, "%s:%" APR_OFF_T_FMT, path, offset);
}




/* Functions for working with shared transaction data. */

/* Return the transaction object for transaction TXN_ID from the
   transaction list of filesystem FS (which must already be locked via the
   txn_list_lock mutex).  If the transaction does not exist in the list,
   then create a new transaction object and return it (if CREATE_NEW is
   true) or return NULL (otherwise). */
static fs_fs_shared_txn_data_t *
get_shared_txn(svn_fs_t *fs, const char *txn_id, svn_boolean_t create_new)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  fs_fs_shared_data_t *ffsd = ffd->shared;
  fs_fs_shared_txn_data_t *txn;

  for (txn = ffsd->txns; txn; txn = txn->next)
    if (strcmp(txn->txn_id, txn_id) == 0)
      break;

  if (txn || !create_new)
    return txn;

  /* Use the transaction object from the (single-object) freelist,
     if one is available, or otherwise create a new object. */
  if (ffsd->free_txn)
    {
      txn = ffsd->free_txn;
      ffsd->free_txn = NULL;
    }
  else
    {
      apr_pool_t *subpool = svn_pool_create(ffsd->common_pool);
      txn = apr_palloc(subpool, sizeof(*txn));
      txn->pool = subpool;
    }

  assert(strlen(txn_id) < sizeof(txn->txn_id));
  apr_cpystrn(txn->txn_id, txn_id, sizeof(txn->txn_id));
  txn->being_written = FALSE;

  /* Link this transaction into the head of the list.  We will typically
     be dealing with only one active transaction at a time, so it makes
     sense for searches through the transaction list to look at the
     newest transactions first.  */
  txn->next = ffsd->txns;
  ffsd->txns = txn;

  return txn;
}

/* Free the transaction object for transaction TXN_ID, and remove it
   from the transaction list of filesystem FS (which must already be
   locked via the txn_list_lock mutex).  Do nothing if the transaction
   does not exist. */
static void
free_shared_txn(svn_fs_t *fs, const char *txn_id)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  fs_fs_shared_data_t *ffsd = ffd->shared;
  fs_fs_shared_txn_data_t *txn, *prev = NULL;

  for (txn = ffsd->txns; txn; prev = txn, txn = txn->next)
    if (strcmp(txn->txn_id, txn_id) == 0)
      break;

  if (!txn)
    return;

  if (prev)
    prev->next = txn->next;
  else
    ffsd->txns = txn->next;

  /* As we typically will be dealing with one transaction after another,
     we will maintain a single-object free list so that we can hopefully
     keep reusing the same transaction object. */
  if (!ffsd->free_txn)
    ffsd->free_txn = txn;
  else
    svn_pool_destroy(txn->pool);
}


/* Obtain a lock on the transaction list of filesystem FS, call BODY
   with FS, BATON, and POOL, and then unlock the transaction list.
   Return what BODY returned. */
static svn_error_t *
with_txnlist_lock(svn_fs_t *fs,
                  svn_error_t *(*body)(svn_fs_t *fs,
                                       const void *baton,
                                       apr_pool_t *pool),
                  const void *baton,
                  apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  fs_fs_shared_data_t *ffsd = ffd->shared;

  SVN_MUTEX__WITH_LOCK(ffsd->txn_list_lock,
                       body(fs, baton, pool));

  return SVN_NO_ERROR;
}


/* Get a lock on empty file LOCK_FILENAME, creating it in POOL. */
static svn_error_t *
get_lock_on_filesystem(const char *lock_filename,
                       apr_pool_t *pool)
{
  svn_error_t *err = svn_io_file_lock2(lock_filename, TRUE, FALSE, pool);

  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      /* No lock file?  No big deal; these are just empty files
         anyway.  Create it and try again. */
      svn_error_clear(err);
      err = NULL;

      SVN_ERR(svn_io_file_create(lock_filename, "", pool));
      SVN_ERR(svn_io_file_lock2(lock_filename, TRUE, FALSE, pool));
    }

  return svn_error_trace(err);
}

/* Reset the HAS_WRITE_LOCK member in the FFD given as BATON_VOID.
   When registered with the pool holding the lock on the lock file,
   this makes sure the flag gets reset just before we release the lock. */
static apr_status_t
reset_lock_flag(void *baton_void)
{
  fs_fs_data_t *ffd = baton_void;
  ffd->has_write_lock = FALSE;
  return APR_SUCCESS;
}

/* Obtain a write lock on the file LOCK_FILENAME (protecting with
   LOCK_MUTEX if APR is threaded) in a subpool of POOL, call BODY with
   BATON and that subpool, destroy the subpool (releasing the write
   lock) and return what BODY returned.  If IS_GLOBAL_LOCK is set,
   set the HAS_WRITE_LOCK flag while we keep the write lock. */
static svn_error_t *
with_some_lock_file(svn_fs_t *fs,
                    svn_error_t *(*body)(void *baton,
                                         apr_pool_t *pool),
                    void *baton,
                    const char *lock_filename,
                    svn_boolean_t is_global_lock,
                    apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_error_t *err = get_lock_on_filesystem(lock_filename, subpool);

  if (!err)
    {
      fs_fs_data_t *ffd = fs->fsap_data;

      if (is_global_lock)
        {
          /* set the "got the lock" flag and register reset function */
          apr_pool_cleanup_register(subpool,
                                    ffd,
                                    reset_lock_flag,
                                    apr_pool_cleanup_null);
          ffd->has_write_lock = TRUE;
        }

      /* nobody else will modify the repo state
         => read HEAD & pack info once */
      if (ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
        SVN_ERR(svn_fs_fs__update_min_unpacked_rev(fs, pool));
      SVN_ERR(get_youngest(&ffd->youngest_rev_cache, fs->path,
                           pool));
      err = body(baton, subpool);
    }

  svn_pool_destroy(subpool);

  return svn_error_trace(err);
}

svn_error_t *
svn_fs_fs__with_write_lock(svn_fs_t *fs,
                           svn_error_t *(*body)(void *baton,
                                                apr_pool_t *pool),
                           void *baton,
                           apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  fs_fs_shared_data_t *ffsd = ffd->shared;

  SVN_MUTEX__WITH_LOCK(ffsd->fs_write_lock,
                       with_some_lock_file(fs, body, baton,
                                           path_lock(fs, pool),
                                           TRUE,
                                           pool));

  return SVN_NO_ERROR;
}

/* Run BODY (with BATON and POOL) while the txn-current file
   of FS is locked. */
static svn_error_t *
with_txn_current_lock(svn_fs_t *fs,
                      svn_error_t *(*body)(void *baton,
                                           apr_pool_t *pool),
                      void *baton,
                      apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  fs_fs_shared_data_t *ffsd = ffd->shared;

  SVN_MUTEX__WITH_LOCK(ffsd->txn_current_lock,
                       with_some_lock_file(fs, body, baton,
                                           path_txn_current_lock(fs, pool),
                                           FALSE,
                                           pool));

  return SVN_NO_ERROR;
}

/* A structure used by unlock_proto_rev() and unlock_proto_rev_body(),
   which see. */
struct unlock_proto_rev_baton
{
  const char *txn_id;
  void *lockcookie;
};

/* Callback used in the implementation of unlock_proto_rev(). */
static svn_error_t *
unlock_proto_rev_body(svn_fs_t *fs, const void *baton, apr_pool_t *pool)
{
  const struct unlock_proto_rev_baton *b = baton;
  const char *txn_id = b->txn_id;
  apr_file_t *lockfile = b->lockcookie;
  fs_fs_shared_txn_data_t *txn = get_shared_txn(fs, txn_id, FALSE);
  apr_status_t apr_err;

  if (!txn)
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Can't unlock unknown transaction '%s'"),
                             txn_id);
  if (!txn->being_written)
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Can't unlock nonlocked transaction '%s'"),
                             txn_id);

  apr_err = apr_file_unlock(lockfile);
  if (apr_err)
    return svn_error_wrap_apr
      (apr_err,
       _("Can't unlock prototype revision lockfile for transaction '%s'"),
       txn_id);
  apr_err = apr_file_close(lockfile);
  if (apr_err)
    return svn_error_wrap_apr
      (apr_err,
       _("Can't close prototype revision lockfile for transaction '%s'"),
       txn_id);

  txn->being_written = FALSE;

  return SVN_NO_ERROR;
}

/* Unlock the prototype revision file for transaction TXN_ID in filesystem
   FS using cookie LOCKCOOKIE.  The original prototype revision file must
   have been closed _before_ calling this function.

   Perform temporary allocations in POOL. */
static svn_error_t *
unlock_proto_rev(svn_fs_t *fs, const char *txn_id, void *lockcookie,
                 apr_pool_t *pool)
{
  struct unlock_proto_rev_baton b;

  b.txn_id = txn_id;
  b.lockcookie = lockcookie;
  return with_txnlist_lock(fs, unlock_proto_rev_body, &b, pool);
}

/* Same as unlock_proto_rev(), but requires that the transaction list
   lock is already held. */
static svn_error_t *
unlock_proto_rev_list_locked(svn_fs_t *fs, const char *txn_id,
                             void *lockcookie,
                             apr_pool_t *pool)
{
  struct unlock_proto_rev_baton b;

  b.txn_id = txn_id;
  b.lockcookie = lockcookie;
  return unlock_proto_rev_body(fs, &b, pool);
}

/* A structure used by get_writable_proto_rev() and
   get_writable_proto_rev_body(), which see. */
struct get_writable_proto_rev_baton
{
  apr_file_t **file;
  void **lockcookie;
  const char *txn_id;
};

/* Callback used in the implementation of get_writable_proto_rev(). */
static svn_error_t *
get_writable_proto_rev_body(svn_fs_t *fs, const void *baton, apr_pool_t *pool)
{
  const struct get_writable_proto_rev_baton *b = baton;
  apr_file_t **file = b->file;
  void **lockcookie = b->lockcookie;
  const char *txn_id = b->txn_id;
  svn_error_t *err;
  fs_fs_shared_txn_data_t *txn = get_shared_txn(fs, txn_id, TRUE);

  /* First, ensure that no thread in this process (including this one)
     is currently writing to this transaction's proto-rev file. */
  if (txn->being_written)
    return svn_error_createf(SVN_ERR_FS_REP_BEING_WRITTEN, NULL,
                             _("Cannot write to the prototype revision file "
                               "of transaction '%s' because a previous "
                               "representation is currently being written by "
                               "this process"),
                             txn_id);


  /* We know that no thread in this process is writing to the proto-rev
     file, and by extension, that no thread in this process is holding a
     lock on the prototype revision lock file.  It is therefore safe
     for us to attempt to lock this file, to see if any other process
     is holding a lock. */

  {
    apr_file_t *lockfile;
    apr_status_t apr_err;
    const char *lockfile_path = path_txn_proto_rev_lock(fs, txn_id, pool);

    /* Open the proto-rev lockfile, creating it if necessary, as it may
       not exist if the transaction dates from before the lockfiles were
       introduced.

       ### We'd also like to use something like svn_io_file_lock2(), but
           that forces us to create a subpool just to be able to unlock
           the file, which seems a waste. */
    SVN_ERR(svn_io_file_open(&lockfile, lockfile_path,
                             APR_WRITE | APR_CREATE, APR_OS_DEFAULT, pool));

    apr_err = apr_file_lock(lockfile,
                            APR_FLOCK_EXCLUSIVE | APR_FLOCK_NONBLOCK);
    if (apr_err)
      {
        svn_error_clear(svn_io_file_close(lockfile, pool));

        if (APR_STATUS_IS_EAGAIN(apr_err))
          return svn_error_createf(SVN_ERR_FS_REP_BEING_WRITTEN, NULL,
                                   _("Cannot write to the prototype revision "
                                     "file of transaction '%s' because a "
                                     "previous representation is currently "
                                     "being written by another process"),
                                   txn_id);

        return svn_error_wrap_apr(apr_err,
                                  _("Can't get exclusive lock on file '%s'"),
                                  svn_dirent_local_style(lockfile_path, pool));
      }

    *lockcookie = lockfile;
  }

  /* We've successfully locked the transaction; mark it as such. */
  txn->being_written = TRUE;


  /* Now open the prototype revision file and seek to the end. */
  err = svn_io_file_open(file,
                         svn_fs_fs__path_txn_proto_rev(fs, txn_id, pool),
                         APR_WRITE | APR_BUFFERED, APR_OS_DEFAULT, pool);

  /* You might expect that we could dispense with the following seek
     and achieve the same thing by opening the file using APR_APPEND.
     Unfortunately, APR's buffered file implementation unconditionally
     places its initial file pointer at the start of the file (even for
     files opened with APR_APPEND), so we need this seek to reconcile
     the APR file pointer to the OS file pointer (since we need to be
     able to read the current file position later). */
  if (!err)
    {
      apr_off_t offset = 0;
      err = svn_io_file_seek(*file, APR_END, &offset, pool);
    }

  if (err)
    {
      err = svn_error_compose_create(
              err,
              unlock_proto_rev_list_locked(fs, txn_id, *lockcookie, pool));

      *lockcookie = NULL;
    }

  return svn_error_trace(err);
}

/* Get a handle to the prototype revision file for transaction TXN_ID in
   filesystem FS, and lock it for writing.  Return FILE, a file handle
   positioned at the end of the file, and LOCKCOOKIE, a cookie that
   should be passed to unlock_proto_rev() to unlock the file once FILE
   has been closed.

   If the prototype revision file is already locked, return error
   SVN_ERR_FS_REP_BEING_WRITTEN.

   Perform all allocations in POOL. */
static svn_error_t *
get_writable_proto_rev(apr_file_t **file,
                       void **lockcookie,
                       svn_fs_t *fs, const char *txn_id,
                       apr_pool_t *pool)
{
  struct get_writable_proto_rev_baton b;

  b.file = file;
  b.lockcookie = lockcookie;
  b.txn_id = txn_id;

  return with_txnlist_lock(fs, get_writable_proto_rev_body, &b, pool);
}

/* Callback used in the implementation of purge_shared_txn(). */
static svn_error_t *
purge_shared_txn_body(svn_fs_t *fs, const void *baton, apr_pool_t *pool)
{
  const char *txn_id = baton;

  free_shared_txn(fs, txn_id);
  svn_fs_fs__reset_txn_caches(fs);

  return SVN_NO_ERROR;
}

/* Purge the shared data for transaction TXN_ID in filesystem FS.
   Perform all allocations in POOL. */
static svn_error_t *
purge_shared_txn(svn_fs_t *fs, const char *txn_id, apr_pool_t *pool)
{
  return with_txnlist_lock(fs, purge_shared_txn_body, txn_id, pool);
}



/* Check that BUF, a nul-terminated buffer of text from format file PATH,
   contains only digits at OFFSET and beyond, raising an error if not.

   Uses POOL for temporary allocation. */
static svn_error_t *
check_format_file_buffer_numeric(const char *buf, apr_off_t offset,
                                 const char *path, apr_pool_t *pool)
{
  return svn_fs_fs__check_file_buffer_numeric(buf, offset, path, "Format",
                                              pool);
}

/* Read the format number and maximum number of files per directory
   from PATH and return them in *PFORMAT and *MAX_FILES_PER_DIR
   respectively.

   *MAX_FILES_PER_DIR is obtained from the 'layout' format option, and
   will be set to zero if a linear scheme should be used.

   Use POOL for temporary allocation. */
static svn_error_t *
read_format(int *pformat, int *max_files_per_dir,
            const char *path, apr_pool_t *pool)
{
  svn_error_t *err;
  svn_stream_t *stream;
  svn_stringbuf_t *content;
  svn_stringbuf_t *buf;
  svn_boolean_t eos = FALSE;

  err = svn_stringbuf_from_file2(&content, path, pool);
  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      /* Treat an absent format file as format 1.  Do not try to
         create the format file on the fly, because the repository
         might be read-only for us, or this might be a read-only
         operation, and the spirit of FSFS is to make no changes
         whatseover in read-only operations.  See thread starting at
         http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=97600
         for more. */
      svn_error_clear(err);
      *pformat = 1;
      *max_files_per_dir = 0;

      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  stream = svn_stream_from_stringbuf(content, pool);
  SVN_ERR(svn_stream_readline(stream, &buf, "\n", &eos, pool));
  if (buf->len == 0 && eos)
    {
      /* Return a more useful error message. */
      return svn_error_createf(SVN_ERR_BAD_VERSION_FILE_FORMAT, NULL,
                               _("Can't read first line of format file '%s'"),
                               svn_dirent_local_style(path, pool));
    }

  /* Check that the first line contains only digits. */
  SVN_ERR(check_format_file_buffer_numeric(buf->data, 0, path, pool));
  SVN_ERR(svn_cstring_atoi(pformat, buf->data));

  /* Set the default values for anything that can be set via an option. */
  *max_files_per_dir = 0;

  /* Read any options. */
  while (!eos)
    {
      SVN_ERR(svn_stream_readline(stream, &buf, "\n", &eos, pool));
      if (buf->len == 0)
        break;

      if (*pformat >= SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT &&
          strncmp(buf->data, "layout ", 7) == 0)
        {
          if (strcmp(buf->data + 7, "linear") == 0)
            {
              *max_files_per_dir = 0;
              continue;
            }

          if (strncmp(buf->data + 7, "sharded ", 8) == 0)
            {
              /* Check that the argument is numeric. */
              SVN_ERR(check_format_file_buffer_numeric(buf->data, 15, path, pool));
              SVN_ERR(svn_cstring_atoi(max_files_per_dir, buf->data + 15));
              continue;
            }
        }

      return svn_error_createf(SVN_ERR_BAD_VERSION_FILE_FORMAT, NULL,
         _("'%s' contains invalid filesystem format option '%s'"),
         svn_dirent_local_style(path, pool), buf->data);
    }

  return SVN_NO_ERROR;
}

/* Write the format number and maximum number of files per directory
   to a new format file in PATH, possibly expecting to overwrite a
   previously existing file.

   Use POOL for temporary allocation. */
static svn_error_t *
write_format(const char *path, int format, int max_files_per_dir,
             svn_boolean_t overwrite, apr_pool_t *pool)
{
  svn_stringbuf_t *sb;

  SVN_ERR_ASSERT(1 <= format && format <= SVN_FS_FS__FORMAT_NUMBER);

  sb = svn_stringbuf_createf(pool, "%d\n", format);

  if (format >= SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT)
    {
      if (max_files_per_dir)
        svn_stringbuf_appendcstr(sb, apr_psprintf(pool, "layout sharded %d\n",
                                                  max_files_per_dir));
      else
        svn_stringbuf_appendcstr(sb, "layout linear\n");
    }

  /* svn_io_write_version_file() does a load of magic to allow it to
     replace version files that already exist.  We only need to do
     that when we're allowed to overwrite an existing file. */
  if (! overwrite)
    {
      /* Create the file */
      SVN_ERR(svn_io_file_create(path, sb->data, pool));
    }
  else
    {
      SVN_ERR(svn_io_write_atomic(path, sb->data, sb->len,
                                  NULL /* copy_perms_path */, pool));
    }

  /* And set the perms to make it read only */
  return svn_io_set_file_read_only(path, FALSE, pool);
}

/* Return the error SVN_ERR_FS_UNSUPPORTED_FORMAT if FS's format
   number is not the same as a format number supported by this
   Subversion. */
static svn_error_t *
check_format(int format)
{
  /* Blacklist.  These formats may be either younger or older than
     SVN_FS_FS__FORMAT_NUMBER, but we don't support them. */
  if (format == SVN_FS_FS__PACKED_REVPROP_SQLITE_DEV_FORMAT)
    return svn_error_createf(SVN_ERR_FS_UNSUPPORTED_FORMAT, NULL,
                             _("Found format '%d', only created by "
                               "unreleased dev builds; see "
                               "http://subversion.apache.org"
                               "/docs/release-notes/1.7#revprop-packing"),
                             format);

  /* We support all formats from 1-current simultaneously */
  if (1 <= format && format <= SVN_FS_FS__FORMAT_NUMBER)
    return SVN_NO_ERROR;

  return svn_error_createf(SVN_ERR_FS_UNSUPPORTED_FORMAT, NULL,
     _("Expected FS format between '1' and '%d'; found format '%d'"),
     SVN_FS_FS__FORMAT_NUMBER, format);
}

svn_boolean_t
svn_fs_fs__fs_supports_mergeinfo(svn_fs_t *fs)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  return ffd->format >= SVN_FS_FS__MIN_MERGEINFO_FORMAT;
}

/* Read the configuration information of the file system at FS_PATH
 * and set the respective values in FFD.  Use POOL for allocations.
 */
static svn_error_t *
read_config(fs_fs_data_t *ffd,
            const char *fs_path,
            apr_pool_t *pool)
{
  SVN_ERR(svn_config_read3(&ffd->config,
                           svn_dirent_join(fs_path, PATH_CONFIG, pool),
                           FALSE, FALSE, FALSE, pool));

  /* Initialize ffd->rep_sharing_allowed. */
  if (ffd->format >= SVN_FS_FS__MIN_REP_SHARING_FORMAT)
    SVN_ERR(svn_config_get_bool(ffd->config, &ffd->rep_sharing_allowed,
                                CONFIG_SECTION_REP_SHARING,
                                CONFIG_OPTION_ENABLE_REP_SHARING, TRUE));
  else
    ffd->rep_sharing_allowed = FALSE;

  /* Initialize deltification settings in ffd. */
  if (ffd->format >= SVN_FS_FS__MIN_DELTIFICATION_FORMAT)
    {
      SVN_ERR(svn_config_get_bool(ffd->config, &ffd->deltify_directories,
                                  CONFIG_SECTION_DELTIFICATION,
                                  CONFIG_OPTION_ENABLE_DIR_DELTIFICATION,
                                  FALSE));
      SVN_ERR(svn_config_get_bool(ffd->config, &ffd->deltify_properties,
                                  CONFIG_SECTION_DELTIFICATION,
                                  CONFIG_OPTION_ENABLE_PROPS_DELTIFICATION,
                                  FALSE));
      SVN_ERR(svn_config_get_int64(ffd->config, &ffd->max_deltification_walk,
                                   CONFIG_SECTION_DELTIFICATION,
                                   CONFIG_OPTION_MAX_DELTIFICATION_WALK,
                                   SVN_FS_FS_MAX_DELTIFICATION_WALK));
      SVN_ERR(svn_config_get_int64(ffd->config, &ffd->max_linear_deltification,
                                   CONFIG_SECTION_DELTIFICATION,
                                   CONFIG_OPTION_MAX_LINEAR_DELTIFICATION,
                                   SVN_FS_FS_MAX_LINEAR_DELTIFICATION));
    }
  else
    {
      ffd->deltify_directories = FALSE;
      ffd->deltify_properties = FALSE;
      ffd->max_deltification_walk = SVN_FS_FS_MAX_DELTIFICATION_WALK;
      ffd->max_linear_deltification = SVN_FS_FS_MAX_LINEAR_DELTIFICATION;
    }

  /* Initialize revprop packing settings in ffd. */
  if (ffd->format >= SVN_FS_FS__MIN_PACKED_REVPROP_FORMAT)
    {
      SVN_ERR(svn_config_get_bool(ffd->config, &ffd->compress_packed_revprops,
                                  CONFIG_SECTION_PACKED_REVPROPS,
                                  CONFIG_OPTION_COMPRESS_PACKED_REVPROPS,
                                  FALSE));
      SVN_ERR(svn_config_get_int64(ffd->config, &ffd->revprop_pack_size,
                                   CONFIG_SECTION_PACKED_REVPROPS,
                                   CONFIG_OPTION_REVPROP_PACK_SIZE,
                                   ffd->compress_packed_revprops
                                       ? 0x100
                                       : 0x40));

      ffd->revprop_pack_size *= 1024;
    }
  else
    {
      ffd->revprop_pack_size = 0x10000;
      ffd->compress_packed_revprops = FALSE;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
write_config(svn_fs_t *fs,
             apr_pool_t *pool)
{
#define NL APR_EOL_STR
  static const char * const fsfs_conf_contents =
"### This file controls the configuration of the FSFS filesystem."           NL
""                                                                           NL
"[" SVN_CACHE_CONFIG_CATEGORY_MEMCACHED_SERVERS "]"                          NL
"### These options name memcached servers used to cache internal FSFS"       NL
"### data.  See http://www.danga.com/memcached/ for more information on"     NL
"### memcached.  To use memcached with FSFS, run one or more memcached"      NL
"### servers, and specify each of them as an option like so:"                NL
"# first-server = 127.0.0.1:11211"                                           NL
"# remote-memcached = mymemcached.corp.example.com:11212"                    NL
"### The option name is ignored; the value is of the form HOST:PORT."        NL
"### memcached servers can be shared between multiple repositories;"         NL
"### however, if you do this, you *must* ensure that repositories have"      NL
"### distinct UUIDs and paths, or else cached data from one repository"      NL
"### might be used by another accidentally.  Note also that memcached has"   NL
"### no authentication for reads or writes, so you must ensure that your"    NL
"### memcached servers are only accessible by trusted users."                NL
""                                                                           NL
"[" CONFIG_SECTION_CACHES "]"                                                NL
"### When a cache-related error occurs, normally Subversion ignores it"      NL
"### and continues, logging an error if the server is appropriately"         NL
"### configured (and ignoring it with file:// access).  To make"             NL
"### Subversion never ignore cache errors, uncomment this line."             NL
"# " CONFIG_OPTION_FAIL_STOP " = true"                                       NL
""                                                                           NL
"[" CONFIG_SECTION_REP_SHARING "]"                                           NL
"### To conserve space, the filesystem can optionally avoid storing"         NL
"### duplicate representations.  This comes at a slight cost in"             NL
"### performance, as maintaining a database of shared representations can"   NL
"### increase commit times.  The space savings are dependent upon the size"  NL
"### of the repository, the number of objects it contains and the amount of" NL
"### duplication between them, usually a function of the branching and"      NL
"### merging process."                                                       NL
"###"                                                                        NL
"### The following parameter enables rep-sharing in the repository.  It can" NL
"### be switched on and off at will, but for best space-saving results"      NL
"### should be enabled consistently over the life of the repository."        NL
"### 'svnadmin verify' will check the rep-cache regardless of this setting." NL
"### rep-sharing is enabled by default."                                     NL
"# " CONFIG_OPTION_ENABLE_REP_SHARING " = true"                              NL
""                                                                           NL
"[" CONFIG_SECTION_DELTIFICATION "]"                                         NL
"### To conserve space, the filesystem stores data as differences against"   NL
"### existing representations.  This comes at a slight cost in performance," NL
"### as calculating differences can increase commit times.  Reading data"    NL
"### will also create higher CPU load and the data will be fragmented."      NL
"### Since deltification tends to save significant amounts of disk space,"   NL
"### the overall I/O load can actually be lower."                            NL
"###"                                                                        NL
"### The options in this section allow for tuning the deltification"         NL
"### strategy.  Their effects on data size and server performance may vary"  NL
"### from one repository to another.  Versions prior to 1.8 will ignore"     NL
"### this section."                                                          NL
"###"                                                                        NL
"### The following parameter enables deltification for directories. It can"  NL
"### be switched on and off at will, but for best space-saving results"      NL
"### should be enabled consistently over the life of the repository."        NL
"### Repositories containing large directories will benefit greatly."        NL
"### In rarely read repositories, the I/O overhead may be significant as"    NL
"### cache hit rates will most likely be low"                                NL
"### directory deltification is disabled by default."                        NL
"# " CONFIG_OPTION_ENABLE_DIR_DELTIFICATION " = false"                       NL
"###"                                                                        NL
"### The following parameter enables deltification for properties on files"  NL
"### and directories.  Overall, this is a minor tuning option but can save"  NL
"### some disk space if you merge frequently or frequently change node"      NL
"### properties.  You should not activate this if rep-sharing has been"      NL
"### disabled because this may result in a net increase in repository size." NL
"### property deltification is disabled by default."                         NL
"# " CONFIG_OPTION_ENABLE_PROPS_DELTIFICATION " = false"                     NL
"###"                                                                        NL
"### During commit, the server may need to walk the whole change history of" NL
"### of a given node to find a suitable deltification base.  This linear"    NL
"### process can impact commit times, svnadmin load and similar operations." NL
"### This setting limits the depth of the deltification history.  If the"    NL
"### threshold has been reached, the node will be stored as fulltext and a"  NL
"### new deltification history begins."                                      NL
"### Note, this is unrelated to svn log."                                    NL
"### Very large values rarely provide significant additional savings but"    NL
"### can impact performance greatly - in particular if directory"            NL
"### deltification has been activated.  Very small values may be useful in"  NL
"### repositories that are dominated by large, changing binaries."           NL
"### Should be a power of two minus 1.  A value of 0 will effectively"       NL
"### disable deltification."                                                 NL
"### For 1.8, the default value is 1023; earlier versions have no limit."    NL
"# " CONFIG_OPTION_MAX_DELTIFICATION_WALK " = 1023"                          NL
"###"                                                                        NL
"### The skip-delta scheme used by FSFS tends to repeatably store redundant" NL
"### delta information where a simple delta against the latest version is"   NL
"### often smaller.  By default, 1.8+ will therefore use skip deltas only"   NL
"### after the linear chain of deltas has grown beyond the threshold"        NL
"### specified by this setting."                                             NL
"### Values up to 64 can result in some reduction in repository size for"    NL
"### the cost of quickly increasing I/O and CPU costs. Similarly, smaller"   NL
"### numbers can reduce those costs at the cost of more disk space.  For"    NL
"### rarely read repositories or those containing larger binaries, this may" NL
"### present a better trade-off."                                            NL
"### Should be a power of two.  A value of 1 or smaller will cause the"      NL
"### exclusive use of skip-deltas (as in pre-1.8)."                          NL
"### For 1.8, the default value is 16; earlier versions use 1."              NL
"# " CONFIG_OPTION_MAX_LINEAR_DELTIFICATION " = 16"                          NL
""                                                                           NL
"[" CONFIG_SECTION_PACKED_REVPROPS "]"                                       NL
"### This parameter controls the size (in kBytes) of packed revprop files."  NL
"### Revprops of consecutive revisions will be concatenated into a single"   NL
"### file up to but not exceeding the threshold given here.  However, each"  NL
"### pack file may be much smaller and revprops of a single revision may be" NL
"### much larger than the limit set here.  The threshold will be applied"    NL
"### before optional compression takes place."                               NL
"### Large values will reduce disk space usage at the expense of increased"  NL
"### latency and CPU usage reading and changing individual revprops.  They"  NL
"### become an advantage when revprop caching has been enabled because a"    NL
"### lot of data can be read in one go.  Values smaller than 4 kByte will"   NL
"### not improve latency any further and quickly render revprop packing"     NL
"### ineffective."                                                           NL
"### revprop-pack-size is 64 kBytes by default for non-compressed revprop"   NL
"### pack files and 256 kBytes when compression has been enabled."           NL
"# " CONFIG_OPTION_REVPROP_PACK_SIZE " = 64"                                 NL
"###"                                                                        NL
"### To save disk space, packed revprop files may be compressed.  Standard"  NL
"### revprops tend to allow for very effective compression.  Reading and"    NL
"### even more so writing, become significantly more CPU intensive.  With"   NL
"### revprop caching enabled, the overhead can be offset by reduced I/O"     NL
"### unless you often modify revprops after packing."                        NL
"### Compressing packed revprops is disabled by default."                    NL
"# " CONFIG_OPTION_COMPRESS_PACKED_REVPROPS " = false"                       NL
;
#undef NL
  return svn_io_file_create(svn_dirent_join(fs->path, PATH_CONFIG, pool),
                            fsfs_conf_contents, pool);
}

svn_error_t *
svn_fs_fs__open(svn_fs_t *fs, const char *path, apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_file_t *uuid_file;
  int format, max_files_per_dir;
  char buf[APR_UUID_FORMATTED_LENGTH + 2];
  apr_size_t limit;

  fs->path = apr_pstrdup(fs->pool, path);

  /* Read the FS format number. */
  SVN_ERR(read_format(&format, &max_files_per_dir,
                      path_format(fs, pool), pool));
  SVN_ERR(check_format(format));

  /* Now we've got a format number no matter what. */
  ffd->format = format;
  ffd->max_files_per_dir = max_files_per_dir;

  /* Read in and cache the repository uuid. */
  SVN_ERR(svn_io_file_open(&uuid_file, path_uuid(fs, pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));

  limit = sizeof(buf);
  SVN_ERR(svn_io_read_length_line(uuid_file, buf, &limit, pool));
  fs->uuid = apr_pstrdup(fs->pool, buf);

  SVN_ERR(svn_io_file_close(uuid_file, pool));

  /* Read the min unpacked revision. */
  if (ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
    SVN_ERR(svn_fs_fs__update_min_unpacked_rev(fs, pool));

  /* Read the configuration file. */
  SVN_ERR(read_config(ffd, fs->path, pool));

  return get_youngest(&(ffd->youngest_rev_cache), path, pool);
}

/* Wrapper around svn_io_file_create which ignores EEXIST. */
static svn_error_t *
create_file_ignore_eexist(const char *file,
                          const char *contents,
                          apr_pool_t *pool)
{
  svn_error_t *err = svn_io_file_create(file, contents, pool);
  if (err && APR_STATUS_IS_EEXIST(err->apr_err))
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
    }
  return svn_error_trace(err);
}

/* Baton type bridging svn_fs_fs__upgrade and upgrade_body carrying 
 * parameters over between them. */
struct upgrade_baton_t
{
  svn_fs_t *fs;
  svn_fs_upgrade_notify_t notify_func;
  void *notify_baton;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;
};

static svn_error_t *
upgrade_body(void *baton, apr_pool_t *pool)
{
  struct upgrade_baton_t *upgrade_baton = baton;
  svn_fs_t *fs = upgrade_baton->fs;
  int format, max_files_per_dir;
  const char *format_path = path_format(fs, pool);
  svn_node_kind_t kind;
  svn_boolean_t needs_revprop_shard_cleanup = FALSE;

  /* Read the FS format number and max-files-per-dir setting. */
  SVN_ERR(read_format(&format, &max_files_per_dir, format_path, pool));
  SVN_ERR(check_format(format));

  /* If the config file does not exist, create one. */
  SVN_ERR(svn_io_check_path(svn_dirent_join(fs->path, PATH_CONFIG, pool),
                            &kind, pool));
  switch (kind)
    {
    case svn_node_none:
      SVN_ERR(write_config(fs, pool));
      break;
    case svn_node_file:
      break;
    default:
      return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                               _("'%s' is not a regular file."
                                 " Please move it out of "
                                 "the way and try again"),
                               svn_dirent_join(fs->path, PATH_CONFIG, pool));
    }

  /* If we're already up-to-date, there's nothing else to be done here. */
  if (format == SVN_FS_FS__FORMAT_NUMBER)
    return SVN_NO_ERROR;

  /* If our filesystem predates the existance of the 'txn-current
     file', make that file and its corresponding lock file. */
  if (format < SVN_FS_FS__MIN_TXN_CURRENT_FORMAT)
    {
      SVN_ERR(create_file_ignore_eexist(path_txn_current(fs, pool), "0\n",
                                        pool));
      SVN_ERR(create_file_ignore_eexist(path_txn_current_lock(fs, pool), "",
                                        pool));
    }

  /* If our filesystem predates the existance of the 'txn-protorevs'
     dir, make that directory.  */
  if (format < SVN_FS_FS__MIN_PROTOREVS_DIR_FORMAT)
    {
      /* We don't use path_txn_proto_rev() here because it expects
         we've already bumped our format. */
      SVN_ERR(svn_io_make_dir_recursively(
          svn_dirent_join(fs->path, PATH_TXN_PROTOS_DIR, pool), pool));
    }

  /* If our filesystem is new enough, write the min unpacked rev file. */
  if (format < SVN_FS_FS__MIN_PACKED_FORMAT)
    SVN_ERR(svn_io_file_create(svn_fs_fs__path_min_unpacked_rev(fs, pool),
                               "0\n", pool));

  /* If the file system supports revision packing but not revprop packing
     *and* the FS has been sharded, pack the revprops up to the point that
     revision data has been packed.  However, keep the non-packed revprop
     files around until after the format bump */
  if (   format >= SVN_FS_FS__MIN_PACKED_FORMAT
      && format < SVN_FS_FS__MIN_PACKED_REVPROP_FORMAT
      && max_files_per_dir > 0)
    {
      needs_revprop_shard_cleanup = TRUE;
      SVN_ERR(svn_fs_fs__upgrade_pack_revprops(fs,
                                               upgrade_baton->notify_func,
                                               upgrade_baton->notify_baton,
                                               upgrade_baton->cancel_func,
                                               upgrade_baton->cancel_baton,
                                               pool));
    }

  /* Bump the format file. */
  SVN_ERR(write_format(format_path, SVN_FS_FS__FORMAT_NUMBER,
                       max_files_per_dir, TRUE, pool));
  if (upgrade_baton->notify_func)
    SVN_ERR(upgrade_baton->notify_func(upgrade_baton->notify_baton,
                                       SVN_FS_FS__FORMAT_NUMBER,
                                       svn_fs_upgrade_format_bumped,
                                       pool));

  /* Now, it is safe to remove the redundant revprop files. */
  if (needs_revprop_shard_cleanup)
    SVN_ERR(svn_fs_fs__upgrade_cleanup_pack_revprops(fs,
                                               upgrade_baton->notify_func,
                                               upgrade_baton->notify_baton,
                                               upgrade_baton->cancel_func,
                                               upgrade_baton->cancel_baton,
                                               pool));

  /* Done */
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__upgrade(svn_fs_t *fs,
                   svn_fs_upgrade_notify_t notify_func,
                   void *notify_baton,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *pool)
{
  struct upgrade_baton_t baton;
  baton.fs = fs;
  baton.notify_func = notify_func;
  baton.notify_baton = notify_baton;
  baton.cancel_func = cancel_func;
  baton.cancel_baton = cancel_baton;
  
  return svn_fs_fs__with_write_lock(fs, upgrade_body, (void *)&baton, pool);
}

/* Find the youngest revision in a repository at path FS_PATH and
   return it in *YOUNGEST_P.  Perform temporary allocations in
   POOL. */
static svn_error_t *
get_youngest(svn_revnum_t *youngest_p,
             const char *fs_path,
             apr_pool_t *pool)
{
  svn_stringbuf_t *buf;
  SVN_ERR(svn_fs_fs__read_content(&buf,
                                  svn_dirent_join(fs_path, PATH_CURRENT,
                                                  pool),
                                  pool));

  *youngest_p = SVN_STR_TO_REV(buf->data);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__youngest_rev(svn_revnum_t *youngest_p,
                        svn_fs_t *fs,
                        apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;

  SVN_ERR(get_youngest(youngest_p, fs->path, pool));
  ffd->youngest_rev_cache = *youngest_p;

  return SVN_NO_ERROR;
}

/* Return SVN_ERR_FS_NO_SUCH_REVISION if the given revision is newer
   than the current youngest revision or is simply not a valid
   revision number, else return success.

   FSFS is based around the concept that commits only take effect when
   the number in "current" is bumped.  Thus if there happens to be a rev
   or revprops file installed for a revision higher than the one recorded
   in "current" (because a commit failed between installing the rev file
   and bumping "current", or because an administrator rolled back the
   repository by resetting "current" without deleting rev files, etc), it
   ought to be completely ignored.  This function provides the check
   by which callers can make that decision. */
static svn_error_t *
ensure_revision_exists(svn_fs_t *fs,
                       svn_revnum_t rev,
                       apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;

  if (! SVN_IS_VALID_REVNUM(rev))
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("Invalid revision number '%ld'"), rev);


  /* Did the revision exist the last time we checked the current
     file? */
  if (rev <= ffd->youngest_rev_cache)
    return SVN_NO_ERROR;

  SVN_ERR(get_youngest(&(ffd->youngest_rev_cache), fs->path, pool));

  /* Check again. */
  if (rev <= ffd->youngest_rev_cache)
    return SVN_NO_ERROR;

  return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                           _("No such revision %ld"), rev);
}

svn_error_t *
svn_fs_fs__revision_exists(svn_revnum_t rev,
                           svn_fs_t *fs,
                           apr_pool_t *pool)
{
  /* Different order of parameters. */
  SVN_ERR(ensure_revision_exists(fs, rev, pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__put_node_revision(svn_fs_t *fs,
                             const svn_fs_id_t *id,
                             node_revision_t *noderev,
                             svn_boolean_t fresh_txn_root,
                             apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_file_t *noderev_file;
  const char *txn_id = svn_fs_fs__id_txn_id(id);

  noderev->is_fresh_txn_root = fresh_txn_root;

  if (! txn_id)
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Attempted to write to non-transaction '%s'"),
                             svn_fs_fs__id_unparse(id, pool)->data);

  SVN_ERR(svn_io_file_open(&noderev_file,
                           svn_fs_fs__path_txn_node_rev(fs, id, pool),
                           APR_WRITE | APR_CREATE | APR_TRUNCATE
                           | APR_BUFFERED, APR_OS_DEFAULT, pool));

  SVN_ERR(svn_fs_fs__write_noderev(svn_stream_from_aprfile2(noderev_file, TRUE,
                                                            pool),
                                   noderev, ffd->format,
                                   svn_fs_fs__fs_supports_mergeinfo(fs),
                                   pool));

  SVN_ERR(svn_io_file_close(noderev_file, pool));

  return SVN_NO_ERROR;
}

/* For the in-transaction NODEREV within FS, write the sha1->rep mapping
 * file in the respective transaction, if rep sharing has been enabled etc.
 * Use POOL for temporary allocations.
 */
static svn_error_t *
store_sha1_rep_mapping(svn_fs_t *fs,
                       node_revision_t *noderev,
                       apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;

  /* if rep sharing has been enabled and the noderev has a data rep and
   * its SHA-1 is known, store the rep struct under its SHA1. */
  if (   ffd->rep_sharing_allowed
      && noderev->data_rep
      && noderev->data_rep->sha1_checksum)
    {
      apr_file_t *rep_file;
      const char *file_name = path_txn_sha1(fs,
                                            svn_fs_fs__id_txn_id(noderev->id),
                                            noderev->data_rep->sha1_checksum,
                                            pool);
      svn_stringbuf_t *rep_string
        = svn_fs_fs__unparse_representation(noderev->data_rep,
                                            ffd->format,
                                            (noderev->kind == svn_node_dir),
                                            FALSE,
                                            pool);
      SVN_ERR(svn_io_file_open(&rep_file, file_name,
                               APR_WRITE | APR_CREATE | APR_TRUNCATE
                               | APR_BUFFERED, APR_OS_DEFAULT, pool));

      SVN_ERR(svn_io_file_write_full(rep_file, rep_string->data,
                                     rep_string->len, NULL, pool));

      SVN_ERR(svn_io_file_close(rep_file, pool));
    }

  return SVN_NO_ERROR;
}

static const char *
unparse_dir_entry(svn_node_kind_t kind, const svn_fs_id_t *id,
                  apr_pool_t *pool)
{
  return apr_psprintf(pool, "%s %s",
                      (kind == svn_node_file) ? SVN_FS_FS__KIND_FILE
                                              : SVN_FS_FS__KIND_DIR,
                      svn_fs_fs__id_unparse(id, pool)->data);
}

/* Given a hash ENTRIES of dirent structions, return a hash in
   *STR_ENTRIES_P, that has svn_string_t as the values in the format
   specified by the fs_fs directory contents file.  Perform
   allocations in POOL. */
static svn_error_t *
unparse_dir_entries(apr_hash_t **str_entries_p,
                    apr_hash_t *entries,
                    apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  /* For now, we use a our own hash function to ensure that we get a
   * (largely) stable order when serializing the data.  It also gives
   * us some performance improvement.
   *
   * ### TODO ###
   * Use some sorted or other fixed order data container.
   */
  *str_entries_p = svn_hash__make(pool);

  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      svn_fs_dirent_t *dirent = svn__apr_hash_index_val(hi);
      const char *new_val;

      apr_hash_this(hi, &key, &klen, NULL);
      new_val = unparse_dir_entry(dirent->kind, dirent->id, pool);
      apr_hash_set(*str_entries_p, key, klen,
                   svn_string_create(new_val, pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__file_length(svn_filesize_t *length,
                       node_revision_t *noderev,
                       apr_pool_t *pool)
{
  if (noderev->data_rep)
    *length = noderev->data_rep->expanded_size;
  else
    *length = 0;

  return SVN_NO_ERROR;
}

svn_boolean_t
svn_fs_fs__noderev_same_rep_key(representation_t *a,
                                representation_t *b)
{
  if (a == b)
    return TRUE;

  if (a == NULL || b == NULL)
    return FALSE;

  if (a->offset != b->offset)
    return FALSE;

  if (a->revision != b->revision)
    return FALSE;

  if (a->uniquifier == b->uniquifier)
    return TRUE;

  if (a->uniquifier == NULL || b->uniquifier == NULL)
    return FALSE;

  return strcmp(a->uniquifier, b->uniquifier) == 0;
}

svn_error_t *
svn_fs_fs__file_checksum(svn_checksum_t **checksum,
                         node_revision_t *noderev,
                         svn_checksum_kind_t kind,
                         apr_pool_t *pool)
{
  if (noderev->data_rep)
    {
      switch(kind)
        {
          case svn_checksum_md5:
            *checksum = svn_checksum_dup(noderev->data_rep->md5_checksum,
                                         pool);
            break;
          case svn_checksum_sha1:
            *checksum = svn_checksum_dup(noderev->data_rep->sha1_checksum,
                                         pool);
            break;
          default:
            *checksum = NULL;
        }
    }
  else
    *checksum = NULL;

  return SVN_NO_ERROR;
}

representation_t *
svn_fs_fs__rep_copy(representation_t *rep,
                    apr_pool_t *pool)
{
  representation_t *rep_new;

  if (rep == NULL)
    return NULL;

  rep_new = apr_pcalloc(pool, sizeof(*rep_new));

  memcpy(rep_new, rep, sizeof(*rep_new));
  rep_new->md5_checksum = svn_checksum_dup(rep->md5_checksum, pool);
  rep_new->sha1_checksum = svn_checksum_dup(rep->sha1_checksum, pool);
  rep_new->uniquifier = apr_pstrdup(pool, rep->uniquifier);

  return rep_new;
}

/* Merge the internal-use-only CHANGE into a hash of public-FS
   svn_fs_path_change2_t CHANGES, collapsing multiple changes into a
   single summarical (is that real word?) change per path.  Also keep
   the COPYFROM_CACHE up to date with new adds and replaces.  */
static svn_error_t *
fold_change(apr_hash_t *changes,
            const change_t *change,
            apr_hash_t *copyfrom_cache)
{
  apr_pool_t *pool = apr_hash_pool_get(changes);
  svn_fs_path_change2_t *old_change, *new_change;
  const char *path;
  apr_size_t path_len = strlen(change->path);

  if ((old_change = apr_hash_get(changes, change->path, path_len)))
    {
      /* This path already exists in the hash, so we have to merge
         this change into the already existing one. */

      /* Sanity check:  only allow NULL node revision ID in the
         `reset' case. */
      if ((! change->noderev_id) && (change->kind != svn_fs_path_change_reset))
        return svn_error_create
          (SVN_ERR_FS_CORRUPT, NULL,
           _("Missing required node revision ID"));

      /* Sanity check: we should be talking about the same node
         revision ID as our last change except where the last change
         was a deletion. */
      if (change->noderev_id
          && (! svn_fs_fs__id_eq(old_change->node_rev_id, change->noderev_id))
          && (old_change->change_kind != svn_fs_path_change_delete))
        return svn_error_create
          (SVN_ERR_FS_CORRUPT, NULL,
           _("Invalid change ordering: new node revision ID "
             "without delete"));

      /* Sanity check: an add, replacement, or reset must be the first
         thing to follow a deletion. */
      if ((old_change->change_kind == svn_fs_path_change_delete)
          && (! ((change->kind == svn_fs_path_change_replace)
                 || (change->kind == svn_fs_path_change_reset)
                 || (change->kind == svn_fs_path_change_add))))
        return svn_error_create
          (SVN_ERR_FS_CORRUPT, NULL,
           _("Invalid change ordering: non-add change on deleted path"));

      /* Sanity check: an add can't follow anything except
         a delete or reset.  */
      if ((change->kind == svn_fs_path_change_add)
          && (old_change->change_kind != svn_fs_path_change_delete)
          && (old_change->change_kind != svn_fs_path_change_reset))
        return svn_error_create
          (SVN_ERR_FS_CORRUPT, NULL,
           _("Invalid change ordering: add change on preexisting path"));

      /* Now, merge that change in. */
      switch (change->kind)
        {
        case svn_fs_path_change_reset:
          /* A reset here will simply remove the path change from the
             hash. */
          old_change = NULL;
          break;

        case svn_fs_path_change_delete:
          if (old_change->change_kind == svn_fs_path_change_add)
            {
              /* If the path was introduced in this transaction via an
                 add, and we are deleting it, just remove the path
                 altogether. */
              old_change = NULL;
            }
          else
            {
              /* A deletion overrules all previous changes. */
              old_change->change_kind = svn_fs_path_change_delete;
              old_change->text_mod = change->text_mod;
              old_change->prop_mod = change->prop_mod;
              old_change->copyfrom_rev = SVN_INVALID_REVNUM;
              old_change->copyfrom_path = NULL;
            }
          break;

        case svn_fs_path_change_add:
        case svn_fs_path_change_replace:
          /* An add at this point must be following a previous delete,
             so treat it just like a replace. */
          old_change->change_kind = svn_fs_path_change_replace;
          old_change->node_rev_id = svn_fs_fs__id_copy(change->noderev_id,
                                                       pool);
          old_change->text_mod = change->text_mod;
          old_change->prop_mod = change->prop_mod;
          if (change->copyfrom_rev == SVN_INVALID_REVNUM)
            {
              old_change->copyfrom_rev = SVN_INVALID_REVNUM;
              old_change->copyfrom_path = NULL;
            }
          else
            {
              old_change->copyfrom_rev = change->copyfrom_rev;
              old_change->copyfrom_path = apr_pstrdup(pool,
                                                      change->copyfrom_path);
            }
          break;

        case svn_fs_path_change_modify:
        default:
          if (change->text_mod)
            old_change->text_mod = TRUE;
          if (change->prop_mod)
            old_change->prop_mod = TRUE;
          break;
        }

      /* Point our new_change to our (possibly modified) old_change. */
      new_change = old_change;
    }
  else
    {
      /* This change is new to the hash, so make a new public change
         structure from the internal one (in the hash's pool), and dup
         the path into the hash's pool, too. */
      new_change = apr_pcalloc(pool, sizeof(*new_change));
      new_change->node_rev_id = svn_fs_fs__id_copy(change->noderev_id, pool);
      new_change->change_kind = change->kind;
      new_change->text_mod = change->text_mod;
      new_change->prop_mod = change->prop_mod;
      /* In FSFS, copyfrom_known is *always* true, since we've always
       * stored copyfroms in changed paths lists. */
      new_change->copyfrom_known = TRUE;
      if (change->copyfrom_rev != SVN_INVALID_REVNUM)
        {
          new_change->copyfrom_rev = change->copyfrom_rev;
          new_change->copyfrom_path = apr_pstrdup(pool, change->copyfrom_path);
        }
      else
        {
          new_change->copyfrom_rev = SVN_INVALID_REVNUM;
          new_change->copyfrom_path = NULL;
        }
    }

  if (new_change)
    new_change->node_kind = change->node_kind;

  /* Add (or update) this path.

     Note: this key might already be present, and it would be nice to
     re-use its value, but there is no way to fetch it. The API makes no
     guarantees that this (new) key will not be retained. Thus, we (again)
     copy the key into the target pool to ensure a proper lifetime.  */
  path = apr_pstrmemdup(pool, change->path, path_len);
  apr_hash_set(changes, path, path_len, new_change);

  /* Update the copyfrom cache, if any. */
  if (copyfrom_cache)
    {
      apr_pool_t *copyfrom_pool = apr_hash_pool_get(copyfrom_cache);
      const char *copyfrom_string = NULL, *copyfrom_key = path;
      if (new_change)
        {
          if (SVN_IS_VALID_REVNUM(new_change->copyfrom_rev))
            copyfrom_string = apr_psprintf(copyfrom_pool, "%ld %s",
                                           new_change->copyfrom_rev,
                                           new_change->copyfrom_path);
          else
            copyfrom_string = "";
        }
      /* We need to allocate a copy of the key in the copyfrom_pool if
       * we're not doing a deletion and if it isn't already there. */
      if (   copyfrom_string
          && (   ! apr_hash_count(copyfrom_cache)
              || ! apr_hash_get(copyfrom_cache, copyfrom_key, path_len)))
        copyfrom_key = apr_pstrmemdup(copyfrom_pool, copyfrom_key, path_len);

      apr_hash_set(copyfrom_cache, copyfrom_key, path_len,
                   copyfrom_string);
    }

  return SVN_NO_ERROR;
}

/* Examine all the changed path entries in CHANGES and store them in
   *CHANGED_PATHS.  Folding is done to remove redundant or unnecessary
   *data.  Store a hash of paths to copyfrom "REV PATH" strings in
   COPYFROM_HASH if it is non-NULL.  If PREFOLDED is true, assume that
   the changed-path entries have already been folded (by
   write_final_changed_path_info) and may be out of order, so we shouldn't
   remove children of replaced or deleted directories.  Do all
   allocations in POOL. */
static svn_error_t *
process_changes(apr_hash_t *changed_paths,
                apr_hash_t *copyfrom_cache,
                apr_array_header_t *changes,
                svn_boolean_t prefolded,
                apr_pool_t *pool)
{
  apr_pool_t *iterpool = svn_pool_create(pool);
  int i;

  /* Read in the changes one by one, folding them into our local hash
     as necessary. */

  for (i = 0; i < changes->nelts; ++i)
    {
      change_t *change = APR_ARRAY_IDX(changes, i, change_t *);

      SVN_ERR(fold_change(changed_paths, change, copyfrom_cache));

      /* Now, if our change was a deletion or replacement, we have to
         blow away any changes thus far on paths that are (or, were)
         children of this path.
         ### i won't bother with another iteration pool here -- at
         most we talking about a few extra dups of paths into what
         is already a temporary subpool.
      */

      if (((change->kind == svn_fs_path_change_delete)
           || (change->kind == svn_fs_path_change_replace))
          && ! prefolded)
        {
          apr_hash_index_t *hi;

          /* a potential child path must contain at least 2 more chars
             (the path separator plus at least one char for the name).
             Also, we should not assume that all paths have been normalized
             i.e. some might have trailing path separators.
          */
          apr_ssize_t change_path_len = strlen(change->path);
          apr_ssize_t min_child_len = change_path_len == 0
                                    ? 1
                                    : change->path[change_path_len-1] == '/'
                                        ? change_path_len + 1
                                        : change_path_len + 2;

          /* CAUTION: This is the inner loop of an O(n^2) algorithm.
             The number of changes to process may be >> 1000.
             Therefore, keep the inner loop as tight as possible.
          */
          for (hi = apr_hash_first(iterpool, changed_paths);
               hi;
               hi = apr_hash_next(hi))
            {
              /* KEY is the path. */
              const void *path;
              apr_ssize_t klen;
              apr_hash_this(hi, &path, &klen, NULL);

              /* If we come across a child of our path, remove it.
                 Call svn_dirent_is_child only if there is a chance that
                 this is actually a sub-path.
               */
              if (   klen >= min_child_len
                  && svn_dirent_is_child(change->path, path, iterpool))
                apr_hash_set(changed_paths, path, klen, NULL);
            }
        }

      /* Clear the per-iteration subpool. */
      svn_pool_clear(iterpool);
    }

  /* Destroy the per-iteration subpool. */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__txn_changes_fetch(apr_hash_t **changed_paths_p,
                             svn_fs_t *fs,
                             const char *txn_id,
                             apr_pool_t *pool)
{
  apr_file_t *file;
  apr_hash_t *changed_paths = apr_hash_make(pool);
  apr_array_header_t *changes;
  apr_pool_t *scratch_pool = svn_pool_create(pool);

  SVN_ERR(svn_io_file_open(&file, path_txn_changes(fs, txn_id, pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));

  SVN_ERR(svn_fs_fs__read_changes(&changes,
                                  svn_stream_from_aprfile2(file, TRUE,
                                                           scratch_pool),
                                  scratch_pool));
  SVN_ERR(process_changes(changed_paths, NULL, changes, FALSE, pool));
  svn_pool_destroy(scratch_pool);

  SVN_ERR(svn_io_file_close(file, pool));

  *changed_paths_p = changed_paths;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__paths_changed(apr_hash_t **changed_paths_p,
                         svn_fs_t *fs,
                         svn_revnum_t rev,
                         apr_hash_t *copyfrom_cache,
                         apr_pool_t *pool)
{
  apr_hash_t *changed_paths;
  apr_array_header_t *changes;
  apr_pool_t *scratch_pool = svn_pool_create(pool);

  SVN_ERR(svn_fs_fs__get_changes(&changes, fs, rev, scratch_pool));

  changed_paths = svn_hash__make(pool);

  SVN_ERR(process_changes(changed_paths, copyfrom_cache, changes,
                          TRUE, pool));
  svn_pool_destroy(scratch_pool);

  *changed_paths_p = changed_paths;

  return SVN_NO_ERROR;
}

/* Copy a revision node-rev SRC into the current transaction TXN_ID in
   the filesystem FS.  This is only used to create the root of a transaction.
   Allocations are from POOL.  */
static svn_error_t *
create_new_txn_noderev_from_rev(svn_fs_t *fs,
                                const char *txn_id,
                                svn_fs_id_t *src,
                                apr_pool_t *pool)
{
  node_revision_t *noderev;
  const char *node_id, *copy_id;

  SVN_ERR(svn_fs_fs__get_node_revision(&noderev, fs, src, pool));

  if (svn_fs_fs__id_txn_id(noderev->id))
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Copying from transactions not allowed"));

  noderev->predecessor_id = noderev->id;
  noderev->predecessor_count++;
  noderev->copyfrom_path = NULL;
  noderev->copyfrom_rev = SVN_INVALID_REVNUM;

  /* For the transaction root, the copyroot never changes. */

  node_id = svn_fs_fs__id_node_id(noderev->id);
  copy_id = svn_fs_fs__id_copy_id(noderev->id);
  noderev->id = svn_fs_fs__id_txn_create(node_id, copy_id, txn_id, pool);

  return svn_fs_fs__put_node_revision(fs, noderev->id, noderev, TRUE, pool);
}

/* A structure used by get_and_increment_txn_key_body(). */
struct get_and_increment_txn_key_baton {
  svn_fs_t *fs;
  char *txn_id;
  apr_pool_t *pool;
};

/* Callback used in the implementation of create_txn_dir().  This gets
   the current base 36 value in PATH_TXN_CURRENT and increments it.
   It returns the original value by the baton. */
static svn_error_t *
get_and_increment_txn_key_body(void *baton, apr_pool_t *pool)
{
  struct get_and_increment_txn_key_baton *cb = baton;
  const char *txn_current_filename = path_txn_current(cb->fs, pool);
  char next_txn_id[MAX_KEY_SIZE+3];
  apr_size_t len;

  svn_stringbuf_t *buf;
  SVN_ERR(svn_fs_fs__read_content(&buf, txn_current_filename, cb->pool));

  /* remove trailing newlines */
  svn_stringbuf_strip_whitespace(buf);
  cb->txn_id = buf->data;
  len = buf->len;

  /* Increment the key and add a trailing \n to the string so the
     txn-current file has a newline in it. */
  svn_fs_fs__next_key(cb->txn_id, &len, next_txn_id);
  next_txn_id[len] = '\n';
  ++len;
  next_txn_id[len] = '\0';

  SVN_ERR(svn_io_write_atomic(txn_current_filename, next_txn_id, len,
                              txn_current_filename /* copy_perms path */, pool));

  return SVN_NO_ERROR;
}

/* Create a unique directory for a transaction in FS based on revision
   REV.  Return the ID for this transaction in *ID_P.  Use a sequence
   value in the transaction ID to prevent reuse of transaction IDs. */
static svn_error_t *
create_txn_dir(const char **id_p, svn_fs_t *fs, svn_revnum_t rev,
               apr_pool_t *pool)
{
  struct get_and_increment_txn_key_baton cb;
  const char *txn_dir;

  /* Get the current transaction sequence value, which is a base-36
     number, from the txn-current file, and write an
     incremented value back out to the file.  Place the revision
     number the transaction is based off into the transaction id. */
  cb.pool = pool;
  cb.fs = fs;
  SVN_ERR(with_txn_current_lock(fs,
                                get_and_increment_txn_key_body,
                                &cb,
                                pool));
  *id_p = apr_psprintf(pool, "%ld-%s", rev, cb.txn_id);

  txn_dir = svn_dirent_join_many(pool,
                                 fs->path,
                                 PATH_TXNS_DIR,
                                 apr_pstrcat(pool, *id_p, PATH_EXT_TXN,
                                             (char *)NULL),
                                 NULL);

  return svn_io_dir_make(txn_dir, APR_OS_DEFAULT, pool);
}

/* Create a unique directory for a transaction in FS based on revision
   REV.  Return the ID for this transaction in *ID_P.  This
   implementation is used in svn 1.4 and earlier repositories and is
   kept in 1.5 and greater to support the --pre-1.4-compatible and
   --pre-1.5-compatible repository creation options.  Reused
   transaction IDs are possible with this implementation. */
static svn_error_t *
create_txn_dir_pre_1_5(const char **id_p, svn_fs_t *fs, svn_revnum_t rev,
                       apr_pool_t *pool)
{
  unsigned int i;
  apr_pool_t *subpool;
  const char *unique_path, *prefix;

  /* Try to create directories named "<txndir>/<rev>-<uniqueifier>.txn". */
  prefix = svn_dirent_join_many(pool, fs->path, PATH_TXNS_DIR,
                                apr_psprintf(pool, "%ld", rev), NULL);

  subpool = svn_pool_create(pool);
  for (i = 1; i <= 99999; i++)
    {
      svn_error_t *err;

      svn_pool_clear(subpool);
      unique_path = apr_psprintf(subpool, "%s-%u" PATH_EXT_TXN, prefix, i);
      err = svn_io_dir_make(unique_path, APR_OS_DEFAULT, subpool);
      if (! err)
        {
          /* We succeeded.  Return the basename minus the ".txn" extension. */
          const char *name = svn_dirent_basename(unique_path, subpool);
          *id_p = apr_pstrndup(pool, name,
                               strlen(name) - strlen(PATH_EXT_TXN));
          svn_pool_destroy(subpool);
          return SVN_NO_ERROR;
        }
      if (! APR_STATUS_IS_EEXIST(err->apr_err))
        return svn_error_trace(err);
      svn_error_clear(err);
    }

  return svn_error_createf(SVN_ERR_IO_UNIQUE_NAMES_EXHAUSTED,
                           NULL,
                           _("Unable to create transaction directory "
                             "in '%s' for revision %ld"),
                           svn_dirent_local_style(fs->path, pool),
                           rev);
}

svn_error_t *
svn_fs_fs__create_txn(svn_fs_txn_t **txn_p,
                      svn_fs_t *fs,
                      svn_revnum_t rev,
                      apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_fs_txn_t *txn;
  svn_fs_id_t *root_id;

  txn = apr_pcalloc(pool, sizeof(*txn));

  /* Get the txn_id. */
  if (ffd->format >= SVN_FS_FS__MIN_TXN_CURRENT_FORMAT)
    SVN_ERR(create_txn_dir(&txn->id, fs, rev, pool));
  else
    SVN_ERR(create_txn_dir_pre_1_5(&txn->id, fs, rev, pool));

  txn->fs = fs;
  txn->base_rev = rev;

  txn->vtable = &txn_vtable;
  *txn_p = txn;

  /* Create a new root node for this transaction. */
  SVN_ERR(svn_fs_fs__rev_get_root(&root_id, fs, rev, pool));
  SVN_ERR(create_new_txn_noderev_from_rev(fs, txn->id, root_id, pool));

  /* Create an empty rev file. */
  SVN_ERR(svn_io_file_create(svn_fs_fs__path_txn_proto_rev(fs, txn->id, pool),
                             "", pool));

  /* Create an empty rev-lock file. */
  SVN_ERR(svn_io_file_create(path_txn_proto_rev_lock(fs, txn->id, pool), "",
                             pool));

  /* Create an empty changes file. */
  SVN_ERR(svn_io_file_create(path_txn_changes(fs, txn->id, pool), "",
                             pool));

  /* Create the next-ids file. */
  return svn_io_file_create(path_txn_next_ids(fs, txn->id, pool), "0 0\n",
                            pool);
}

/* Store the property list for transaction TXN_ID in PROPLIST.
   Perform temporary allocations in POOL. */
static svn_error_t *
get_txn_proplist(apr_hash_t *proplist,
                 svn_fs_t *fs,
                 const char *txn_id,
                 apr_pool_t *pool)
{
  svn_stream_t *stream;

  /* Check for issue #3696. (When we find and fix the cause, we can change
   * this to an assertion.) */
  if (txn_id == NULL)
    return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                            _("Internal error: a null transaction id was "
                              "passed to get_txn_proplist()"));

  /* Open the transaction properties file. */
  SVN_ERR(svn_stream_open_readonly(&stream, path_txn_props(fs, txn_id, pool),
                                   pool, pool));

  /* Read in the property list. */
  SVN_ERR(svn_hash_read2(proplist, stream, SVN_HASH_TERMINATOR, pool));

  return svn_stream_close(stream);
}

svn_error_t *
svn_fs_fs__change_txn_prop(svn_fs_txn_t *txn,
                           const char *name,
                           const svn_string_t *value,
                           apr_pool_t *pool)
{
  apr_array_header_t *props = apr_array_make(pool, 1, sizeof(svn_prop_t));
  svn_prop_t prop;

  prop.name = name;
  prop.value = value;
  APR_ARRAY_PUSH(props, svn_prop_t) = prop;

  return svn_fs_fs__change_txn_props(txn, props, pool);
}

svn_error_t *
svn_fs_fs__change_txn_props(svn_fs_txn_t *txn,
                            const apr_array_header_t *props,
                            apr_pool_t *pool)
{
  svn_stringbuf_t *buf;
  svn_stream_t *stream;
  apr_hash_t *txn_prop = apr_hash_make(pool);
  int i;
  svn_error_t *err;

  err = get_txn_proplist(txn_prop, txn->fs, txn->id, pool);
  /* Here - and here only - we need to deal with the possibility that the
     transaction property file doesn't yet exist.  The rest of the
     implementation assumes that the file exists, but we're called to set the
     initial transaction properties as the transaction is being created. */
  if (err && (APR_STATUS_IS_ENOENT(err->apr_err)))
    svn_error_clear(err);
  else if (err)
    return svn_error_trace(err);

  for (i = 0; i < props->nelts; i++)
    {
      svn_prop_t *prop = &APR_ARRAY_IDX(props, i, svn_prop_t);

      svn_hash_sets(txn_prop, prop->name, prop->value);
    }

  /* Create a new version of the file and write out the new props. */
  /* Open the transaction properties file. */
  buf = svn_stringbuf_create_ensure(1024, pool);
  stream = svn_stream_from_stringbuf(buf, pool);
  SVN_ERR(svn_hash_write2(txn_prop, stream, SVN_HASH_TERMINATOR, pool));
  SVN_ERR(svn_stream_close(stream));
  SVN_ERR(svn_io_write_atomic(path_txn_props(txn->fs, txn->id, pool),
                              buf->data, buf->len,
                              NULL /* copy_perms_path */, pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__get_txn(transaction_t **txn_p,
                   svn_fs_t *fs,
                   const char *txn_id,
                   apr_pool_t *pool)
{
  transaction_t *txn;
  node_revision_t *noderev;
  svn_fs_id_t *root_id;

  txn = apr_pcalloc(pool, sizeof(*txn));
  txn->proplist = apr_hash_make(pool);

  SVN_ERR(get_txn_proplist(txn->proplist, fs, txn_id, pool));
  root_id = svn_fs_fs__id_txn_create("0", "0", txn_id, pool);

  SVN_ERR(svn_fs_fs__get_node_revision(&noderev, fs, root_id, pool));

  txn->root_id = svn_fs_fs__id_copy(noderev->id, pool);
  txn->base_id = svn_fs_fs__id_copy(noderev->predecessor_id, pool);
  txn->copies = NULL;

  *txn_p = txn;

  return SVN_NO_ERROR;
}

/* Write out the currently available next node_id NODE_ID and copy_id
   COPY_ID for transaction TXN_ID in filesystem FS.  The next node-id is
   used both for creating new unique nodes for the given transaction, as
   well as uniquifying representations.  Perform temporary allocations in
   POOL. */
static svn_error_t *
write_next_ids(svn_fs_t *fs,
               const char *txn_id,
               const char *node_id,
               const char *copy_id,
               apr_pool_t *pool)
{
  apr_file_t *file;
  svn_stream_t *out_stream;

  SVN_ERR(svn_io_file_open(&file, path_txn_next_ids(fs, txn_id, pool),
                           APR_WRITE | APR_TRUNCATE,
                           APR_OS_DEFAULT, pool));

  out_stream = svn_stream_from_aprfile2(file, TRUE, pool);

  SVN_ERR(svn_stream_printf(out_stream, pool, "%s %s\n", node_id, copy_id));

  SVN_ERR(svn_stream_close(out_stream));
  return svn_io_file_close(file, pool);
}

/* Find out what the next unique node-id and copy-id are for
   transaction TXN_ID in filesystem FS.  Store the results in *NODE_ID
   and *COPY_ID.  The next node-id is used both for creating new unique
   nodes for the given transaction, as well as uniquifying representations.
   Perform all allocations in POOL. */
static svn_error_t *
read_next_ids(const char **node_id,
              const char **copy_id,
              svn_fs_t *fs,
              const char *txn_id,
              apr_pool_t *pool)
{
  apr_file_t *file;
  char buf[MAX_KEY_SIZE*2+3];
  apr_size_t limit;
  char *str, *last_str = buf;

  SVN_ERR(svn_io_file_open(&file, path_txn_next_ids(fs, txn_id, pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));

  limit = sizeof(buf);
  SVN_ERR(svn_io_read_length_line(file, buf, &limit, pool));

  SVN_ERR(svn_io_file_close(file, pool));

  /* Parse this into two separate strings. */

  str = svn_cstring_tokenize(" ", &last_str);
  if (! str)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("next-id file corrupt"));

  *node_id = apr_pstrdup(pool, str);

  str = svn_cstring_tokenize(" ", &last_str);
  if (! str)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("next-id file corrupt"));

  *copy_id = apr_pstrdup(pool, str);

  return SVN_NO_ERROR;
}

/* Get a new and unique to this transaction node-id for transaction
   TXN_ID in filesystem FS.  Store the new node-id in *NODE_ID_P.
   Node-ids are guaranteed to be unique to this transction, but may
   not necessarily be sequential.  Perform all allocations in POOL. */
static svn_error_t *
get_new_txn_node_id(const char **node_id_p,
                    svn_fs_t *fs,
                    const char *txn_id,
                    apr_pool_t *pool)
{
  const char *cur_node_id, *cur_copy_id;
  char *node_id;
  apr_size_t len;

  /* First read in the current next-ids file. */
  SVN_ERR(read_next_ids(&cur_node_id, &cur_copy_id, fs, txn_id, pool));

  node_id = apr_pcalloc(pool, strlen(cur_node_id) + 2);

  len = strlen(cur_node_id);
  svn_fs_fs__next_key(cur_node_id, &len, node_id);

  SVN_ERR(write_next_ids(fs, txn_id, node_id, cur_copy_id, pool));

  *node_id_p = apr_pstrcat(pool, "_", cur_node_id, (char *)NULL);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__create_node(const svn_fs_id_t **id_p,
                       svn_fs_t *fs,
                       node_revision_t *noderev,
                       const char *copy_id,
                       const char *txn_id,
                       apr_pool_t *pool)
{
  const char *node_id;
  const svn_fs_id_t *id;

  /* Get a new node-id for this node. */
  SVN_ERR(get_new_txn_node_id(&node_id, fs, txn_id, pool));

  id = svn_fs_fs__id_txn_create(node_id, copy_id, txn_id, pool);

  noderev->id = id;

  SVN_ERR(svn_fs_fs__put_node_revision(fs, noderev->id, noderev, FALSE, pool));

  *id_p = id;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__purge_txn(svn_fs_t *fs,
                     const char *txn_id,
                     apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;

  /* Remove the shared transaction object associated with this transaction. */
  SVN_ERR(purge_shared_txn(fs, txn_id, pool));
  /* Remove the directory associated with this transaction. */
  SVN_ERR(svn_io_remove_dir2(svn_fs_fs__path_txn_dir(fs, txn_id, pool),
                             FALSE, NULL, NULL, pool));
  if (ffd->format >= SVN_FS_FS__MIN_PROTOREVS_DIR_FORMAT)
    {
      /* Delete protorev and its lock, which aren't in the txn
         directory.  It's OK if they don't exist (for example, if this
         is post-commit and the proto-rev has been moved into
         place). */
      SVN_ERR(svn_io_remove_file2(
                  svn_fs_fs__path_txn_proto_rev(fs, txn_id, pool),
                  TRUE, pool));
      SVN_ERR(svn_io_remove_file2(path_txn_proto_rev_lock(fs, txn_id, pool),
                                  TRUE, pool));
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__abort_txn(svn_fs_txn_t *txn,
                     apr_pool_t *pool)
{
  SVN_ERR(svn_fs__check_fs(txn->fs, TRUE));

  /* Now, purge the transaction. */
  SVN_ERR_W(svn_fs_fs__purge_txn(txn->fs, txn->id, pool),
            apr_psprintf(pool, _("Transaction '%s' cleanup failed"),
                         txn->id));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__set_entry(svn_fs_t *fs,
                     const char *txn_id,
                     node_revision_t *parent_noderev,
                     const char *name,
                     const svn_fs_id_t *id,
                     svn_node_kind_t kind,
                     apr_pool_t *pool)
{
  representation_t *rep = parent_noderev->data_rep;
  const char *filename
    = svn_fs_fs__path_txn_node_children(fs, parent_noderev->id, pool);
  apr_file_t *file;
  svn_stream_t *out;
  fs_fs_data_t *ffd = fs->fsap_data;
  apr_pool_t *subpool = svn_pool_create(pool);

  if (!rep || !rep->txn_id)
    {
      const char *unique_suffix;
      apr_hash_t *entries;

      /* Before we can modify the directory, we need to dump its old
         contents into a mutable representation file. */
      SVN_ERR(svn_fs_fs__rep_contents_dir(&entries, fs, parent_noderev,
                                          subpool));
      SVN_ERR(unparse_dir_entries(&entries, entries, subpool));
      SVN_ERR(svn_io_file_open(&file, filename,
                               APR_WRITE | APR_CREATE | APR_BUFFERED,
                               APR_OS_DEFAULT, pool));
      out = svn_stream_from_aprfile2(file, TRUE, pool);
      SVN_ERR(svn_hash_write2(entries, out, SVN_HASH_TERMINATOR, subpool));

      svn_pool_clear(subpool);

      /* Mark the node-rev's data rep as mutable. */
      rep = apr_pcalloc(pool, sizeof(*rep));
      rep->revision = SVN_INVALID_REVNUM;
      rep->txn_id = txn_id;
      SVN_ERR(get_new_txn_node_id(&unique_suffix, fs, txn_id, pool));
      rep->uniquifier = apr_psprintf(pool, "%s/%s", txn_id, unique_suffix);
      parent_noderev->data_rep = rep;
      SVN_ERR(svn_fs_fs__put_node_revision(fs, parent_noderev->id,
                                           parent_noderev, FALSE, pool));
    }
  else
    {
      /* The directory rep is already mutable, so just open it for append. */
      SVN_ERR(svn_io_file_open(&file, filename, APR_WRITE | APR_APPEND,
                               APR_OS_DEFAULT, pool));
      out = svn_stream_from_aprfile2(file, TRUE, pool);
    }

  /* if we have a directory cache for this transaction, update it */
  if (ffd->txn_dir_cache)
    {
      /* build parameters: (name, new entry) pair */
      const char *key =
          svn_fs_fs__id_unparse(parent_noderev->id, subpool)->data;
      replace_baton_t baton;

      baton.name = name;
      baton.new_entry = NULL;

      if (id)
        {
          baton.new_entry = apr_pcalloc(subpool, sizeof(*baton.new_entry));
          baton.new_entry->name = name;
          baton.new_entry->kind = kind;
          baton.new_entry->id = id;
        }

      /* actually update the cached directory (if cached) */
      SVN_ERR(svn_cache__set_partial(ffd->txn_dir_cache, key,
                                     svn_fs_fs__replace_dir_entry, &baton,
                                     subpool));
    }
  svn_pool_clear(subpool);

  /* Append an incremental hash entry for the entry change. */
  if (id)
    {
      const char *val = unparse_dir_entry(kind, id, subpool);

      SVN_ERR(svn_stream_printf(out, subpool, "K %" APR_SIZE_T_FMT "\n%s\n"
                                "V %" APR_SIZE_T_FMT "\n%s\n",
                                strlen(name), name,
                                strlen(val), val));
    }
  else
    {
      SVN_ERR(svn_stream_printf(out, subpool, "D %" APR_SIZE_T_FMT "\n%s\n",
                                strlen(name), name));
    }

  SVN_ERR(svn_io_file_close(file, subpool));
  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__add_change(svn_fs_t *fs,
                      const char *txn_id,
                      const char *path,
                      const svn_fs_id_t *id,
                      svn_fs_path_change_kind_t change_kind,
                      svn_boolean_t text_mod,
                      svn_boolean_t prop_mod,
                      svn_node_kind_t node_kind,
                      svn_revnum_t copyfrom_rev,
                      const char *copyfrom_path,
                      apr_pool_t *pool)
{
  apr_file_t *file;
  svn_fs_path_change2_t *change;
  apr_hash_t *changes = apr_hash_make(pool);

  SVN_ERR(svn_io_file_open(&file, path_txn_changes(fs, txn_id, pool),
                           APR_APPEND | APR_WRITE | APR_CREATE
                           | APR_BUFFERED, APR_OS_DEFAULT, pool));

  change = svn_fs__path_change_create_internal(id, change_kind, pool);
  change->text_mod = text_mod;
  change->prop_mod = prop_mod;
  change->node_kind = node_kind;
  change->copyfrom_rev = copyfrom_rev;
  change->copyfrom_path = apr_pstrdup(pool, copyfrom_path);

  svn_hash_sets(changes, path, change);
  SVN_ERR(svn_fs_fs__write_changes(svn_stream_from_aprfile2(file, TRUE, pool),
                                   fs, changes, FALSE, pool));

  return svn_io_file_close(file, pool);
}

/* This baton is used by the representation writing streams.  It keeps
   track of the checksum information as well as the total size of the
   representation so far. */
struct rep_write_baton
{
  /* The FS we are writing to. */
  svn_fs_t *fs;

  /* Actual file to which we are writing. */
  svn_stream_t *rep_stream;

  /* A stream from the delta combiner.  Data written here gets
     deltified, then eventually written to rep_stream. */
  svn_stream_t *delta_stream;

  /* Where is this representation header stored. */
  apr_off_t rep_offset;

  /* Start of the actual data. */
  apr_off_t delta_start;

  /* How many bytes have been written to this rep already. */
  svn_filesize_t rep_size;

  /* The node revision for which we're writing out info. */
  node_revision_t *noderev;

  /* Actual output file. */
  apr_file_t *file;
  /* Lock 'cookie' used to unlock the output file once we've finished
     writing to it. */
  void *lockcookie;

  svn_checksum_ctx_t *md5_checksum_ctx;
  svn_checksum_ctx_t *sha1_checksum_ctx;

  apr_pool_t *pool;

  apr_pool_t *parent_pool;
};

/* Handler for the write method of the representation writable stream.
   BATON is a rep_write_baton, DATA is the data to write, and *LEN is
   the length of this data. */
static svn_error_t *
rep_write_contents(void *baton,
                   const char *data,
                   apr_size_t *len)
{
  struct rep_write_baton *b = baton;

  SVN_ERR(svn_checksum_update(b->md5_checksum_ctx, data, *len));
  SVN_ERR(svn_checksum_update(b->sha1_checksum_ctx, data, *len));
  b->rep_size += *len;

  /* If we are writing a delta, use that stream. */
  if (b->delta_stream)
    return svn_stream_write(b->delta_stream, data, len);
  else
    return svn_stream_write(b->rep_stream, data, len);
}

/* Given a node-revision NODEREV in filesystem FS, return the
   representation in *REP to use as the base for a text representation
   delta if PROPS is FALSE.  If PROPS has been set, a suitable props
   base representation will be returned.  Perform temporary allocations
   in *POOL. */
static svn_error_t *
choose_delta_base(representation_t **rep,
                  svn_fs_t *fs,
                  node_revision_t *noderev,
                  svn_boolean_t props,
                  apr_pool_t *pool)
{
  /* The zero-based index (counting from the "oldest" end), along NODEREVs line
   * predecessors, of the node-rev we will use as delta base. */
  int count;
  /* The length of the linear part of a delta chain.  (Delta chains use
   * skip-delta bits for the high-order bits and are linear in the low-order
   * bits.) */
  int walk;
  node_revision_t *base;
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_boolean_t maybe_shared_rep = FALSE;

  /* If we have no predecessors, then use the empty stream as a
     base. */
  if (! noderev->predecessor_count)
    {
      *rep = NULL;
      return SVN_NO_ERROR;
    }

  /* Flip the rightmost '1' bit of the predecessor count to determine
     which file rev (counting from 0) we want to use.  (To see why
     count & (count - 1) unsets the rightmost set bit, think about how
     you decrement a binary number.) */
  count = noderev->predecessor_count;
  count = count & (count - 1);

  /* We use skip delta for limiting the number of delta operations
     along very long node histories.  Close to HEAD however, we create
     a linear history to minimize delta size.  */
  walk = noderev->predecessor_count - count;
  if (walk < (int)ffd->max_linear_deltification)
    count = noderev->predecessor_count - 1;

  /* Finding the delta base over a very long distance can become extremely
     expensive for very deep histories, possibly causing client timeouts etc.
     OTOH, this is a rare operation and its gains are minimal. Lets simply
     start deltification anew close every other 1000 changes or so.  */
  if (walk > (int)ffd->max_deltification_walk)
    {
      *rep = NULL;
      return SVN_NO_ERROR;
    }

  /* Walk back a number of predecessors equal to the difference
     between count and the original predecessor count.  (For example,
     if noderev has ten predecessors and we want the eighth file rev,
     walk back two predecessors.) */
  base = noderev;
  while ((count++) < noderev->predecessor_count)
    {
      SVN_ERR(svn_fs_fs__get_node_revision(&base, fs,
                                           base->predecessor_id, pool));

      /* If there is a shared rep along the way, we need to limit the
       * length of the deltification chain.
       *
       * Please note that copied nodes - such as branch directories - will
       * look the same (false positive) while reps shared within the same
       * revision will not be caught (false negative).
       *
       * Message-ID: <CA+t0gk1wzitkih3GRCLDvK-bTEm=hgppGb_7xXMtvuXDYPfL+Q@mail.gmail.com>
       */
      if (props)
        {
          if (   base->prop_rep
              && svn_fs_fs__id_rev(base->id) > base->prop_rep->revision)
            maybe_shared_rep = TRUE;
        }
      else
        {
          if (   base->data_rep
              && svn_fs_fs__id_rev(base->id) > base->data_rep->revision)
            maybe_shared_rep = TRUE;
        }
    }

  /* return a suitable base representation */
  *rep = props ? base->prop_rep : base->data_rep;

  /* if we encountered a shared rep, its parent chain may be different
   * from the node-rev parent chain. */
  if (*rep && maybe_shared_rep)
    {
      /* Check whether the length of the deltification chain is acceptable.
       * Otherwise, shared reps may form a non-skipping delta chain in
       * extreme cases. */
      int chain_length = 0;
      SVN_ERR(svn_fs_fs__rep_chain_length(&chain_length, *rep, fs, pool));

      /* Some reasonable limit, depending on how acceptable longer linear
       * chains are in this repo.  Also, allow for some minimal chain. */
      if (chain_length >= 2 * (int)ffd->max_linear_deltification + 2)
        *rep = NULL;
    }

  return SVN_NO_ERROR;
}

/* Something went wrong and the pool for the rep write is being
   cleared before we've finished writing the rep.  So we need
   to remove the rep from the protorevfile and we need to unlock
   the protorevfile. */
static apr_status_t
rep_write_cleanup(void *data)
{
  struct rep_write_baton *b = data;
  const char *txn_id = svn_fs_fs__id_txn_id(b->noderev->id);
  svn_error_t *err;

  /* Truncate and close the protorevfile. */
  err = svn_io_file_trunc(b->file, b->rep_offset, b->pool);
  err = svn_error_compose_create(err, svn_io_file_close(b->file, b->pool));

  /* Remove our lock regardless of any preceeding errors so that the
     being_written flag is always removed and stays consistent with the
     file lock which will be removed no matter what since the pool is
     going away. */
  err = svn_error_compose_create(err, unlock_proto_rev(b->fs, txn_id,
                                                       b->lockcookie, b->pool));
  if (err)
    {
      apr_status_t rc = err->apr_err;
      svn_error_clear(err);
      return rc;
    }

  return APR_SUCCESS;
}


/* Get a rep_write_baton and store it in *WB_P for the representation
   indicated by NODEREV in filesystem FS.  Perform allocations in
   POOL.  Only appropriate for file contents, not for props or
   directory contents. */
static svn_error_t *
rep_write_get_baton(struct rep_write_baton **wb_p,
                    svn_fs_t *fs,
                    node_revision_t *noderev,
                    apr_pool_t *pool)
{
  struct rep_write_baton *b;
  apr_file_t *file;
  representation_t *base_rep;
  svn_stream_t *source;
  svn_txdelta_window_handler_t wh;
  void *whb;
  fs_fs_data_t *ffd = fs->fsap_data;
  int diff_version = ffd->format >= SVN_FS_FS__MIN_SVNDIFF1_FORMAT ? 1 : 0;
  svn_fs_fs__rep_header_t header = { 0 };

  b = apr_pcalloc(pool, sizeof(*b));

  b->sha1_checksum_ctx = svn_checksum_ctx_create(svn_checksum_sha1, pool);
  b->md5_checksum_ctx = svn_checksum_ctx_create(svn_checksum_md5, pool);

  b->fs = fs;
  b->parent_pool = pool;
  b->pool = svn_pool_create(pool);
  b->rep_size = 0;
  b->noderev = noderev;

  /* Open the prototype rev file and seek to its end. */
  SVN_ERR(get_writable_proto_rev(&file, &b->lockcookie,
                                 fs, svn_fs_fs__id_txn_id(noderev->id),
                                 b->pool));

  b->file = file;
  b->rep_stream = svn_stream_from_aprfile2(file, TRUE, b->pool);

  SVN_ERR(svn_fs_fs__get_file_offset(&b->rep_offset, file, b->pool));

  /* Get the base for this delta. */
  SVN_ERR(choose_delta_base(&base_rep, fs, noderev, FALSE, b->pool));
  SVN_ERR(svn_fs_fs__get_contents(&source, fs, base_rep, b->pool));

  /* Write out the rep header. */
  if (base_rep)
    {
      header.base_revision = base_rep->revision;
      header.base_offset = base_rep->offset;
      header.base_length = base_rep->size;
      header.type = svn_fs_fs__rep_delta;
    }
  else
    {
      header.type = svn_fs_fs__rep_self_delta;
    }
  SVN_ERR(svn_fs_fs__write_rep_header(&header, b->rep_stream, b->pool));

  /* Now determine the offset of the actual svndiff data. */
  SVN_ERR(svn_fs_fs__get_file_offset(&b->delta_start, file, b->pool));

  /* Cleanup in case something goes wrong. */
  apr_pool_cleanup_register(b->pool, b, rep_write_cleanup,
                            apr_pool_cleanup_null);

  /* Prepare to write the svndiff data. */
  svn_txdelta_to_svndiff3(&wh,
                          &whb,
                          b->rep_stream,
                          diff_version,
                          SVN_DELTA_COMPRESSION_LEVEL_DEFAULT,
                          pool);

  b->delta_stream = svn_txdelta_target_push(wh, whb, source, b->pool);

  *wb_p = b;

  return SVN_NO_ERROR;
}

/* For REP->SHA1_CHECKSUM, try to find an already existing representation
   in FS and return it in *OUT_REP.  If no such representation exists or
   if rep sharing has been disabled for FS, NULL will be returned.  Since
   there may be new duplicate representations within the same uncommitted
   revision, those can be passed in REPS_HASH (maps a sha1 digest onto
   representation_t*), otherwise pass in NULL for REPS_HASH.
   POOL will be used for allocations. The lifetime of the returned rep is
   limited by both, POOL and REP lifetime.
 */
static svn_error_t *
get_shared_rep(representation_t **old_rep,
               svn_fs_t *fs,
               representation_t *rep,
               apr_hash_t *reps_hash,
               apr_pool_t *pool)
{
  svn_error_t *err;
  fs_fs_data_t *ffd = fs->fsap_data;

  /* Return NULL, if rep sharing has been disabled. */
  *old_rep = NULL;
  if (!ffd->rep_sharing_allowed)
    return SVN_NO_ERROR;

  /* Check and see if we already have a representation somewhere that's
     identical to the one we just wrote out.  Start with the hash lookup
     because it is cheepest. */
  if (reps_hash)
    *old_rep = apr_hash_get(reps_hash,
                            rep->sha1_checksum->digest,
                            APR_SHA1_DIGESTSIZE);

  /* If we haven't found anything yet, try harder and consult our DB. */
  if (*old_rep == NULL)
    {
      err = svn_fs_fs__get_rep_reference(old_rep, fs, rep->sha1_checksum,
                                         pool);
      /* ### Other error codes that we shouldn't mask out? */
      if (err == SVN_NO_ERROR)
        {
          if (*old_rep)
            SVN_ERR(svn_fs_fs__check_rep(*old_rep, fs, NULL, NULL, pool));
        }
      else if (err->apr_err == SVN_ERR_FS_CORRUPT
               || SVN_ERROR_IN_CATEGORY(err->apr_err,
                                        SVN_ERR_MALFUNC_CATEGORY_START))
        {
          /* Fatal error; don't mask it.

             In particular, this block is triggered when the rep-cache refers
             to revisions in the future.  We signal that as a corruption situation
             since, once those revisions are less than youngest (because of more
             commits), the rep-cache would be invalid.
           */
          SVN_ERR(err);
        }
      else
        {
          /* Something's wrong with the rep-sharing index.  We can continue
             without rep-sharing, but warn.
           */
          (fs->warning)(fs->warning_baton, err);
          svn_error_clear(err);
          *old_rep = NULL;
        }
    }

  /* look for intra-revision matches (usually data reps but not limited
     to them in case props happen to look like some data rep)
   */
  if (*old_rep == NULL && rep->txn_id)
    {
      svn_node_kind_t kind;
      const char *file_name
        = path_txn_sha1(fs, rep->txn_id, rep->sha1_checksum, pool);

      /* in our txn, is there a rep file named with the wanted SHA1?
         If so, read it and use that rep.
       */
      SVN_ERR(svn_io_check_path(file_name, &kind, pool));
      if (kind == svn_node_file)
        {
          svn_stringbuf_t *rep_string;
          SVN_ERR(svn_stringbuf_from_file2(&rep_string, file_name, pool));
          SVN_ERR(svn_fs_fs__parse_representation(old_rep, rep_string, pool));
        }
    }

  /* Add information that is missing in the cached data. */
  if (*old_rep)
    {
      /* Use the old rep for this content. */
      (*old_rep)->md5_checksum = rep->md5_checksum;
      (*old_rep)->uniquifier = rep->uniquifier;
    }

  return SVN_NO_ERROR;
}

/* Close handler for the representation write stream.  BATON is a
   rep_write_baton.  Writes out a new node-rev that correctly
   references the representation we just finished writing. */
static svn_error_t *
rep_write_contents_close(void *baton)
{
  struct rep_write_baton *b = baton;
  const char *unique_suffix;
  representation_t *rep;
  representation_t *old_rep;
  apr_off_t offset;

  rep = apr_pcalloc(b->parent_pool, sizeof(*rep));
  rep->offset = b->rep_offset;

  /* Close our delta stream so the last bits of svndiff are written
     out. */
  if (b->delta_stream)
    SVN_ERR(svn_stream_close(b->delta_stream));

  /* Determine the length of the svndiff data. */
  SVN_ERR(svn_fs_fs__get_file_offset(&offset, b->file, b->pool));
  rep->size = offset - b->delta_start;

  /* Fill in the rest of the representation field. */
  rep->expanded_size = b->rep_size;
  rep->txn_id = svn_fs_fs__id_txn_id(b->noderev->id);
  SVN_ERR(get_new_txn_node_id(&unique_suffix, b->fs, rep->txn_id, b->pool));
  rep->uniquifier = apr_psprintf(b->parent_pool, "%s/%s", rep->txn_id,
                                 unique_suffix);
  rep->revision = SVN_INVALID_REVNUM;

  /* Finalize the checksum. */
  SVN_ERR(svn_checksum_final(&rep->md5_checksum, b->md5_checksum_ctx,
                              b->parent_pool));
  SVN_ERR(svn_checksum_final(&rep->sha1_checksum, b->sha1_checksum_ctx,
                              b->parent_pool));

  /* Check and see if we already have a representation somewhere that's
     identical to the one we just wrote out. */
  SVN_ERR(get_shared_rep(&old_rep, b->fs, rep, NULL, b->parent_pool));

  if (old_rep)
    {
      /* We need to erase from the protorev the data we just wrote. */
      SVN_ERR(svn_io_file_trunc(b->file, b->rep_offset, b->pool));

      /* Use the old rep for this content. */
      b->noderev->data_rep = old_rep;
    }
  else
    {
      /* Write out our cosmetic end marker. */
      SVN_ERR(svn_stream_puts(b->rep_stream, "ENDREP\n"));

      b->noderev->data_rep = rep;
    }

  /* Remove cleanup callback. */
  apr_pool_cleanup_kill(b->pool, b, rep_write_cleanup);

  /* Write out the new node-rev information. */
  SVN_ERR(svn_fs_fs__put_node_revision(b->fs, b->noderev->id, b->noderev, FALSE,
                                       b->pool));
  if (!old_rep)
    SVN_ERR(store_sha1_rep_mapping(b->fs, b->noderev, b->pool));

  SVN_ERR(svn_io_file_close(b->file, b->pool));
  SVN_ERR(unlock_proto_rev(b->fs, rep->txn_id, b->lockcookie, b->pool));
  svn_pool_destroy(b->pool);

  return SVN_NO_ERROR;
}

/* Store a writable stream in *CONTENTS_P that will receive all data
   written and store it as the file data representation referenced by
   NODEREV in filesystem FS.  Perform temporary allocations in
   POOL.  Only appropriate for file data, not props or directory
   contents. */
static svn_error_t *
set_representation(svn_stream_t **contents_p,
                   svn_fs_t *fs,
                   node_revision_t *noderev,
                   apr_pool_t *pool)
{
  struct rep_write_baton *wb;

  if (! svn_fs_fs__id_txn_id(noderev->id))
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Attempted to write to non-transaction '%s'"),
                             svn_fs_fs__id_unparse(noderev->id, pool)->data);

  SVN_ERR(rep_write_get_baton(&wb, fs, noderev, pool));

  *contents_p = svn_stream_create(wb, pool);
  svn_stream_set_write(*contents_p, rep_write_contents);
  svn_stream_set_close(*contents_p, rep_write_contents_close);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__set_contents(svn_stream_t **stream,
                        svn_fs_t *fs,
                        node_revision_t *noderev,
                        apr_pool_t *pool)
{
  if (noderev->kind != svn_node_file)
    return svn_error_create(SVN_ERR_FS_NOT_FILE, NULL,
                            _("Can't set text contents of a directory"));

  return set_representation(stream, fs, noderev, pool);
}

svn_error_t *
svn_fs_fs__create_successor(const svn_fs_id_t **new_id_p,
                            svn_fs_t *fs,
                            const svn_fs_id_t *old_idp,
                            node_revision_t *new_noderev,
                            const char *copy_id,
                            const char *txn_id,
                            apr_pool_t *pool)
{
  const svn_fs_id_t *id;

  if (! copy_id)
    copy_id = svn_fs_fs__id_copy_id(old_idp);
  id = svn_fs_fs__id_txn_create(svn_fs_fs__id_node_id(old_idp), copy_id,
                                txn_id, pool);

  new_noderev->id = id;

  if (! new_noderev->copyroot_path)
    {
      new_noderev->copyroot_path = apr_pstrdup(pool,
                                               new_noderev->created_path);
      new_noderev->copyroot_rev = svn_fs_fs__id_rev(new_noderev->id);
    }

  SVN_ERR(svn_fs_fs__put_node_revision(fs, new_noderev->id, new_noderev, FALSE,
                                       pool));

  *new_id_p = id;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__set_proplist(svn_fs_t *fs,
                        node_revision_t *noderev,
                        apr_hash_t *proplist,
                        apr_pool_t *pool)
{
  const char *filename
    = svn_fs_fs__path_txn_node_props(fs, noderev->id, pool);
  apr_file_t *file;
  svn_stream_t *out;

  /* Dump the property list to the mutable property file. */
  SVN_ERR(svn_io_file_open(&file, filename,
                           APR_WRITE | APR_CREATE | APR_TRUNCATE
                           | APR_BUFFERED, APR_OS_DEFAULT, pool));
  out = svn_stream_from_aprfile2(file, TRUE, pool);
  SVN_ERR(svn_hash_write2(proplist, out, SVN_HASH_TERMINATOR, pool));
  SVN_ERR(svn_io_file_close(file, pool));

  /* Mark the node-rev's prop rep as mutable, if not already done. */
  if (!noderev->prop_rep || !noderev->prop_rep->txn_id)
    {
      noderev->prop_rep = apr_pcalloc(pool, sizeof(*noderev->prop_rep));
      noderev->prop_rep->txn_id = svn_fs_fs__id_txn_id(noderev->id);
      SVN_ERR(svn_fs_fs__put_node_revision(fs, noderev->id, noderev, FALSE, pool));
    }

  return SVN_NO_ERROR;
}

/* Read the 'current' file for filesystem FS and store the next
   available node id in *NODE_ID, and the next available copy id in
   *COPY_ID.  Allocations are performed from POOL. */
static svn_error_t *
get_next_revision_ids(const char **node_id,
                      const char **copy_id,
                      svn_fs_t *fs,
                      apr_pool_t *pool)
{
  char *buf;
  char *str;
  svn_stringbuf_t *content;

  SVN_ERR(svn_fs_fs__read_content(&content,
                                  svn_fs_fs__path_current(fs, pool),
                                  pool));
  buf = content->data;

  str = svn_cstring_tokenize(" ", &buf);
  if (! str)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Corrupt 'current' file"));

  str = svn_cstring_tokenize(" ", &buf);
  if (! str)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Corrupt 'current' file"));

  *node_id = apr_pstrdup(pool, str);

  str = svn_cstring_tokenize(" \n", &buf);
  if (! str)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Corrupt 'current' file"));

  *copy_id = apr_pstrdup(pool, str);

  return SVN_NO_ERROR;
}

/* This baton is used by the stream created for write_hash_rep. */
struct write_hash_baton
{
  svn_stream_t *stream;

  apr_size_t size;

  svn_checksum_ctx_t *md5_ctx;
  svn_checksum_ctx_t *sha1_ctx;
};

/* The handler for the write_hash_rep stream.  BATON is a
   write_hash_baton, DATA has the data to write and *LEN is the number
   of bytes to write. */
static svn_error_t *
write_hash_handler(void *baton,
                   const char *data,
                   apr_size_t *len)
{
  struct write_hash_baton *whb = baton;

  SVN_ERR(svn_checksum_update(whb->md5_ctx, data, *len));
  SVN_ERR(svn_checksum_update(whb->sha1_ctx, data, *len));

  SVN_ERR(svn_stream_write(whb->stream, data, len));
  whb->size += *len;

  return SVN_NO_ERROR;
}

/* Write out the hash HASH as a text representation to file FILE.  In
   the process, record position, the total size of the dump and MD5 as
   well as SHA1 in REP.   If rep sharing has been enabled and REPS_HASH
   is not NULL, it will be used in addition to the on-disk cache to find
   earlier reps with the same content.  When such existing reps can be
   found, we will truncate the one just written from the file and return
   the existing rep.  Perform temporary allocations in POOL. */
static svn_error_t *
write_hash_rep(representation_t *rep,
               apr_file_t *file,
               apr_hash_t *hash,
               svn_fs_t *fs,
               apr_hash_t *reps_hash,
               apr_pool_t *pool)
{
  svn_stream_t *stream;
  struct write_hash_baton *whb;
  representation_t *old_rep;

  SVN_ERR(svn_fs_fs__get_file_offset(&rep->offset, file, pool));

  whb = apr_pcalloc(pool, sizeof(*whb));

  whb->stream = svn_stream_from_aprfile2(file, TRUE, pool);
  whb->size = 0;
  whb->md5_ctx = svn_checksum_ctx_create(svn_checksum_md5, pool);
  whb->sha1_ctx = svn_checksum_ctx_create(svn_checksum_sha1, pool);

  stream = svn_stream_create(whb, pool);
  svn_stream_set_write(stream, write_hash_handler);

  SVN_ERR(svn_stream_puts(whb->stream, "PLAIN\n"));

  SVN_ERR(svn_hash_write2(hash, stream, SVN_HASH_TERMINATOR, pool));

  /* Store the results. */
  SVN_ERR(svn_checksum_final(&rep->md5_checksum, whb->md5_ctx, pool));
  SVN_ERR(svn_checksum_final(&rep->sha1_checksum, whb->sha1_ctx, pool));

  /* Check and see if we already have a representation somewhere that's
     identical to the one we just wrote out. */
  SVN_ERR(get_shared_rep(&old_rep, fs, rep, reps_hash, pool));

  if (old_rep)
    {
      /* We need to erase from the protorev the data we just wrote. */
      SVN_ERR(svn_io_file_trunc(file, rep->offset, pool));

      /* Use the old rep for this content. */
      memcpy(rep, old_rep, sizeof (*rep));
    }
  else
    {
      /* Write out our cosmetic end marker. */
      SVN_ERR(svn_stream_puts(whb->stream, "ENDREP\n"));

      /* update the representation */
      rep->size = whb->size;
      rep->expanded_size = 0;
    }

  return SVN_NO_ERROR;
}

/* Write out the hash HASH pertaining to the NODEREV in FS as a deltified
   text representation to file FILE.  In the process, record the total size
   and the md5 digest in REP.  If rep sharing has been enabled and REPS_HASH
   is not NULL, it will be used in addition to the on-disk cache to find
   earlier reps with the same content.  When such existing reps can be found,
   we will truncate the one just written from the file and return the existing
   rep.  If PROPS is set, assume that we want to a props representation as
   the base for our delta.  Perform temporary allocations in POOL. */
static svn_error_t *
write_hash_delta_rep(representation_t *rep,
                     apr_file_t *file,
                     apr_hash_t *hash,
                     svn_fs_t *fs,
                     node_revision_t *noderev,
                     apr_hash_t *reps_hash,
                     svn_boolean_t props,
                     apr_pool_t *pool)
{
  svn_txdelta_window_handler_t diff_wh;
  void *diff_whb;

  svn_stream_t *file_stream;
  svn_stream_t *stream;
  representation_t *base_rep;
  representation_t *old_rep;
  svn_stream_t *source;
  svn_fs_fs__rep_header_t header = { 0 };

  apr_off_t rep_end = 0;
  apr_off_t delta_start = 0;

  struct write_hash_baton *whb;
  fs_fs_data_t *ffd = fs->fsap_data;
  int diff_version = ffd->format >= SVN_FS_FS__MIN_SVNDIFF1_FORMAT ? 1 : 0;

  /* Get the base for this delta. */
  SVN_ERR(choose_delta_base(&base_rep, fs, noderev, props, pool));
  SVN_ERR(svn_fs_fs__get_contents(&source, fs, base_rep, pool));

  SVN_ERR(svn_fs_fs__get_file_offset(&rep->offset, file, pool));

  /* Write out the rep header. */
  if (base_rep)
    {
      header.base_revision = base_rep->revision;
      header.base_offset = base_rep->offset;
      header.base_length = base_rep->size;
      header.type = svn_fs_fs__rep_delta;
    }
  else
    {
      header.type = svn_fs_fs__rep_self_delta;
    }

  file_stream = svn_stream_from_aprfile2(file, TRUE, pool);
  SVN_ERR(svn_fs_fs__write_rep_header(&header, file_stream, pool));
  SVN_ERR(svn_fs_fs__get_file_offset(&delta_start, file, pool));

  /* Prepare to write the svndiff data. */
  svn_txdelta_to_svndiff3(&diff_wh,
                          &diff_whb,
                          file_stream,
                          diff_version,
                          SVN_DELTA_COMPRESSION_LEVEL_DEFAULT,
                          pool);

  whb = apr_pcalloc(pool, sizeof(*whb));
  whb->stream = svn_txdelta_target_push(diff_wh, diff_whb, source, pool);
  whb->size = 0;
  whb->md5_ctx = svn_checksum_ctx_create(svn_checksum_md5, pool);
  whb->sha1_ctx = svn_checksum_ctx_create(svn_checksum_sha1, pool);

  /* serialize the hash */
  stream = svn_stream_create(whb, pool);
  svn_stream_set_write(stream, write_hash_handler);

  SVN_ERR(svn_hash_write2(hash, stream, SVN_HASH_TERMINATOR, pool));
  SVN_ERR(svn_stream_close(whb->stream));

  /* Store the results. */
  SVN_ERR(svn_checksum_final(&rep->md5_checksum, whb->md5_ctx, pool));
  SVN_ERR(svn_checksum_final(&rep->sha1_checksum, whb->sha1_ctx, pool));

  /* Check and see if we already have a representation somewhere that's
     identical to the one we just wrote out. */
  SVN_ERR(get_shared_rep(&old_rep, fs, rep, reps_hash, pool));

  if (old_rep)
    {
      /* We need to erase from the protorev the data we just wrote. */
      SVN_ERR(svn_io_file_trunc(file, rep->offset, pool));

      /* Use the old rep for this content. */
      memcpy(rep, old_rep, sizeof (*rep));
    }
  else
    {
      /* Write out our cosmetic end marker. */
      SVN_ERR(svn_fs_fs__get_file_offset(&rep_end, file, pool));
      SVN_ERR(svn_stream_puts(file_stream, "ENDREP\n"));

      /* update the representation */
      rep->expanded_size = whb->size;
      rep->size = rep_end - delta_start;
    }

  return SVN_NO_ERROR;
}

/* Sanity check ROOT_NODEREV, a candidate for being the root node-revision
   of (not yet committed) revision REV in FS.  Use POOL for temporary
   allocations.

   If you change this function, consider updating svn_fs_fs__verify() too.
 */
static svn_error_t *
validate_root_noderev(svn_fs_t *fs,
                      node_revision_t *root_noderev,
                      svn_revnum_t rev,
                      apr_pool_t *pool)
{
  svn_revnum_t head_revnum = rev-1;
  int head_predecessor_count;

  SVN_ERR_ASSERT(rev > 0);

  /* Compute HEAD_PREDECESSOR_COUNT. */
  {
    svn_fs_root_t *head_revision;
    const svn_fs_id_t *head_root_id;
    node_revision_t *head_root_noderev;

    /* Get /@HEAD's noderev. */
    SVN_ERR(svn_fs_fs__revision_root(&head_revision, fs, head_revnum, pool));
    SVN_ERR(svn_fs_fs__node_id(&head_root_id, head_revision, "/", pool));
    SVN_ERR(svn_fs_fs__get_node_revision(&head_root_noderev, fs, head_root_id,
                                         pool));

    head_predecessor_count = head_root_noderev->predecessor_count;
  }

  /* Check that the root noderev's predecessor count equals REV.

     This kind of corruption was seen on svn.apache.org (both on
     the root noderev and on other fspaths' noderevs); see
     issue #4129.

     Normally (rev == root_noderev->predecessor_count), but here we
     use a more roundabout check that should only trigger on new instances
     of the corruption, rather then trigger on each and every new commit
     to a repository that has triggered the bug somewhere in its root
     noderev's history.
   */
  if (root_noderev->predecessor_count != -1
      && (root_noderev->predecessor_count - head_predecessor_count)
         != (rev - head_revnum))
    {
      return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                               _("predecessor count for "
                                 "the root node-revision is wrong: "
                                 "found (%d+%ld != %d), committing r%ld"),
                                 head_predecessor_count,
                                 rev - head_revnum, /* This is equal to 1. */
                                 root_noderev->predecessor_count,
                                 rev);
    }

  return SVN_NO_ERROR;
}

/* Copy a node-revision specified by id ID in fileystem FS from a
   transaction into the proto-rev-file FILE.  Set *NEW_ID_P to a
   pointer to the new node-id which will be allocated in POOL.
   If this is a directory, copy all children as well.

   START_NODE_ID and START_COPY_ID are
   the first available node and copy ids for this filesystem, for older
   FS formats.

   REV is the revision number that this proto-rev-file will represent.

   INITIAL_OFFSET is the offset of the proto-rev-file on entry to
   commit_body.

   If REPS_TO_CACHE is not NULL, append to it a copy (allocated in
   REPS_POOL) of each data rep that is new in this revision.

   If REPS_HASH is not NULL, append copies (allocated in REPS_POOL)
   of the representations of each property rep that is new in this
   revision.

   AT_ROOT is true if the node revision being written is the root
   node-revision.  It is only controls additional sanity checking
   logic.

   Temporary allocations are also from POOL. */
static svn_error_t *
write_final_rev(const svn_fs_id_t **new_id_p,
                apr_file_t *file,
                svn_revnum_t rev,
                svn_fs_t *fs,
                const svn_fs_id_t *id,
                const char *start_node_id,
                const char *start_copy_id,
                apr_off_t initial_offset,
                apr_array_header_t *reps_to_cache,
                apr_hash_t *reps_hash,
                apr_pool_t *reps_pool,
                svn_boolean_t at_root,
                apr_pool_t *pool)
{
  node_revision_t *noderev;
  apr_off_t my_offset;
  char my_node_id_buf[MAX_KEY_SIZE + 2];
  char my_copy_id_buf[MAX_KEY_SIZE + 2];
  const svn_fs_id_t *new_id;
  const char *node_id, *copy_id, *my_node_id, *my_copy_id;
  fs_fs_data_t *ffd = fs->fsap_data;

  *new_id_p = NULL;

  /* Check to see if this is a transaction node. */
  if (! svn_fs_fs__id_txn_id(id))
    return SVN_NO_ERROR;

  SVN_ERR(svn_fs_fs__get_node_revision(&noderev, fs, id, pool));

  if (noderev->kind == svn_node_dir)
    {
      apr_pool_t *subpool;
      apr_hash_t *entries, *str_entries;
      apr_array_header_t *sorted_entries;
      int i;

      /* This is a directory.  Write out all the children first. */
      subpool = svn_pool_create(pool);

      SVN_ERR(svn_fs_fs__rep_contents_dir(&entries, fs, noderev, pool));
      /* For the sake of the repository administrator sort the entries
         so that the final file is deterministic and repeatable,
         however the rest of the FSFS code doesn't require any
         particular order here. */
      sorted_entries = svn_sort__hash(entries, svn_sort_compare_items_lexically,
                                      pool);
      for (i = 0; i < sorted_entries->nelts; ++i)
        {
          svn_fs_dirent_t *dirent = APR_ARRAY_IDX(sorted_entries, i,
                                                  svn_sort__item_t).value;

          svn_pool_clear(subpool);
          SVN_ERR(write_final_rev(&new_id, file, rev, fs, dirent->id,
                                  start_node_id, start_copy_id, initial_offset,
                                  reps_to_cache, reps_hash, reps_pool, FALSE,
                                  subpool));
          if (new_id && (svn_fs_fs__id_rev(new_id) == rev))
            dirent->id = svn_fs_fs__id_copy(new_id, pool);
        }
      svn_pool_destroy(subpool);

      if (noderev->data_rep && noderev->data_rep->txn_id)
        {
          /* Write out the contents of this directory as a text rep. */
          SVN_ERR(unparse_dir_entries(&str_entries, entries, pool));

          noderev->data_rep->txn_id = NULL;
          noderev->data_rep->revision = rev;

          if (ffd->deltify_directories)
            SVN_ERR(write_hash_delta_rep(noderev->data_rep, file,
                                         str_entries, fs, noderev, NULL,
                                         FALSE, pool));
          else
            SVN_ERR(write_hash_rep(noderev->data_rep, file, str_entries,
                                   fs, NULL, pool));
        }
    }
  else
    {
      /* This is a file.  We should make sure the data rep, if it
         exists in a "this" state, gets rewritten to our new revision
         num. */

      if (noderev->data_rep && noderev->data_rep->txn_id)
        {
          noderev->data_rep->txn_id = NULL;
          noderev->data_rep->revision = rev;

          /* See issue 3845.  Some unknown mechanism caused the
             protorev file to get truncated, so check for that
             here.  */
          if (noderev->data_rep->offset + noderev->data_rep->size
              > initial_offset)
            return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                                    _("Truncated protorev file detected"));
        }
    }

  /* Fix up the property reps. */
  if (noderev->prop_rep && noderev->prop_rep->txn_id)
    {
      apr_hash_t *proplist;
      SVN_ERR(svn_fs_fs__get_proplist(&proplist, fs, noderev, pool));

      noderev->prop_rep->txn_id = NULL;
      noderev->prop_rep->revision = rev;

      if (ffd->deltify_properties)
        SVN_ERR(write_hash_delta_rep(noderev->prop_rep, file,
                                     proplist, fs, noderev, reps_hash,
                                     TRUE, pool));
      else
        SVN_ERR(write_hash_rep(noderev->prop_rep, file, proplist,
                               fs, reps_hash, pool));
    }


  /* Convert our temporary ID into a permanent revision one. */
  SVN_ERR(svn_fs_fs__get_file_offset(&my_offset, file, pool));

  node_id = svn_fs_fs__id_node_id(noderev->id);
  if (*node_id == '_')
    {
      if (ffd->format >= SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT)
        my_node_id = apr_psprintf(pool, "%s-%ld", node_id + 1, rev);
      else
        {
          svn_fs_fs__add_keys(start_node_id, node_id + 1, my_node_id_buf);
          my_node_id = my_node_id_buf;
        }
    }
  else
    my_node_id = node_id;

  copy_id = svn_fs_fs__id_copy_id(noderev->id);
  if (*copy_id == '_')
    {
      if (ffd->format >= SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT)
        my_copy_id = apr_psprintf(pool, "%s-%ld", copy_id + 1, rev);
      else
        {
          svn_fs_fs__add_keys(start_copy_id, copy_id + 1, my_copy_id_buf);
          my_copy_id = my_copy_id_buf;
        }
    }
  else
    my_copy_id = copy_id;

  if (noderev->copyroot_rev == SVN_INVALID_REVNUM)
    noderev->copyroot_rev = rev;

  new_id = svn_fs_fs__id_rev_create(my_node_id, my_copy_id, rev, my_offset,
                                    pool);

  noderev->id = new_id;

  if (ffd->rep_sharing_allowed)
    {
      /* Save the data representation's hash in the rep cache. */
      if (   noderev->data_rep && noderev->kind == svn_node_file
          && noderev->data_rep->revision == rev)
        {
          SVN_ERR_ASSERT(reps_to_cache && reps_pool);
          APR_ARRAY_PUSH(reps_to_cache, representation_t *)
            = svn_fs_fs__rep_copy(noderev->data_rep, reps_pool);
        }

      if (noderev->prop_rep && noderev->prop_rep->revision == rev)
        {
          /* Add new property reps to hash and on-disk cache. */
          representation_t *copy
            = svn_fs_fs__rep_copy(noderev->prop_rep, reps_pool);

          SVN_ERR_ASSERT(reps_to_cache && reps_pool);
          APR_ARRAY_PUSH(reps_to_cache, representation_t *) = copy;

          apr_hash_set(reps_hash,
                        copy->sha1_checksum->digest,
                        APR_SHA1_DIGESTSIZE,
                        copy);
        }
    }

  /* don't serialize SHA1 for dirs to disk (waste of space) */
  if (noderev->data_rep && noderev->kind == svn_node_dir)
    noderev->data_rep->sha1_checksum = NULL;

  /* don't serialize SHA1 for props to disk (waste of space) */
  if (noderev->prop_rep)
    noderev->prop_rep->sha1_checksum = NULL;

  /* Workaround issue #4031: is-fresh-txn-root in revision files. */
  noderev->is_fresh_txn_root = FALSE;

  /* Write out our new node-revision. */
  if (at_root)
    SVN_ERR(validate_root_noderev(fs, noderev, rev, pool));

  SVN_ERR(svn_fs_fs__write_noderev(svn_stream_from_aprfile2(file, TRUE, pool),
                                   noderev, ffd->format,
                                   svn_fs_fs__fs_supports_mergeinfo(fs),
                                   pool));

  /* Return our ID that references the revision file. */
  *new_id_p = noderev->id;

  return SVN_NO_ERROR;
}

/* Write the changed path info from transaction TXN_ID in filesystem
   FS to the permanent rev-file FILE.  *OFFSET_P is set the to offset
   in the file of the beginning of this information.  Perform
   temporary allocations in POOL. */
static svn_error_t *
write_final_changed_path_info(apr_off_t *offset_p,
                              apr_file_t *file,
                              svn_fs_t *fs,
                              const char *txn_id,
                              apr_pool_t *pool)
{
  apr_hash_t *changed_paths;
  apr_off_t offset;

  SVN_ERR(svn_fs_fs__get_file_offset(&offset, file, pool));

  SVN_ERR(svn_fs_fs__txn_changes_fetch(&changed_paths, fs, txn_id, pool));

  SVN_ERR(svn_fs_fs__write_changes(svn_stream_from_aprfile2(file, TRUE, pool),
                                   fs, changed_paths, TRUE, pool));

  *offset_p = offset;

  return SVN_NO_ERROR;
}

/* Open a new svn_fs_t handle to FS, set that handle's concept of "current
   youngest revision" to NEW_REV, and call svn_fs_fs__verify_root() on
   NEW_REV's revision root.

   Intended to be called as the very last step in a commit before 'current'
   is bumped.  This implies that we are holding the write lock. */
static svn_error_t *
verify_as_revision_before_current_plus_plus(svn_fs_t *fs,
                                            svn_revnum_t new_rev,
                                            apr_pool_t *pool)
{
#ifdef SVN_DEBUG
  fs_fs_data_t *ffd = fs->fsap_data;
  svn_fs_t *ft; /* fs++ == ft */
  svn_fs_root_t *root;
  fs_fs_data_t *ft_ffd;
  apr_hash_t *fs_config;

  SVN_ERR_ASSERT(ffd->svn_fs_open_);

  /* make sure FT does not simply return data cached by other instances
   * but actually retrieves it from disk at least once.
   */
  fs_config = apr_hash_make(pool);
  svn_hash_sets(fs_config, SVN_FS_CONFIG_FSFS_CACHE_NS,
                           svn_uuid_generate(pool));
  SVN_ERR(ffd->svn_fs_open_(&ft, fs->path,
                            fs_config,
                            pool));
  ft_ffd = ft->fsap_data;
  /* Don't let FT consult rep-cache.db, either. */
  ft_ffd->rep_sharing_allowed = FALSE;

  /* Time travel! */
  ft_ffd->youngest_rev_cache = new_rev;

  SVN_ERR(svn_fs_fs__revision_root(&root, ft, new_rev, pool));
  SVN_ERR_ASSERT(root->is_txn_root == FALSE && root->rev == new_rev);
  SVN_ERR_ASSERT(ft_ffd->youngest_rev_cache == new_rev);
  SVN_ERR(svn_fs_fs__verify_root(root, pool));
#endif /* SVN_DEBUG */

  return SVN_NO_ERROR;
}

/* Update the 'current' file to hold the correct next node and copy_ids
   from transaction TXN_ID in filesystem FS.  The current revision is
   set to REV.  Perform temporary allocations in POOL. */
static svn_error_t *
write_final_current(svn_fs_t *fs,
                    const char *txn_id,
                    svn_revnum_t rev,
                    const char *start_node_id,
                    const char *start_copy_id,
                    apr_pool_t *pool)
{
  const char *txn_node_id, *txn_copy_id;
  char new_node_id[MAX_KEY_SIZE + 2];
  char new_copy_id[MAX_KEY_SIZE + 2];
  fs_fs_data_t *ffd = fs->fsap_data;

  if (ffd->format >= SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT)
    return svn_fs_fs__write_current(fs, rev, NULL, NULL, pool);

  /* To find the next available ids, we add the id that used to be in
     the 'current' file, to the next ids from the transaction file. */
  SVN_ERR(read_next_ids(&txn_node_id, &txn_copy_id, fs, txn_id, pool));

  svn_fs_fs__add_keys(start_node_id, txn_node_id, new_node_id);
  svn_fs_fs__add_keys(start_copy_id, txn_copy_id, new_copy_id);

  return svn_fs_fs__write_current(fs, rev, new_node_id, new_copy_id, pool);
}

/* Verify that the user registed with FS has all the locks necessary to
   permit all the changes associate with TXN_NAME.
   The FS write lock is assumed to be held by the caller. */
static svn_error_t *
verify_locks(svn_fs_t *fs,
             const char *txn_name,
             apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_hash_t *changes;
  apr_hash_index_t *hi;
  apr_array_header_t *changed_paths;
  svn_stringbuf_t *last_recursed = NULL;
  int i;

  /* Fetch the changes for this transaction. */
  SVN_ERR(svn_fs_fs__txn_changes_fetch(&changes, fs, txn_name, pool));

  /* Make an array of the changed paths, and sort them depth-first-ily.  */
  changed_paths = apr_array_make(pool, apr_hash_count(changes) + 1,
                                 sizeof(const char *));
  for (hi = apr_hash_first(pool, changes); hi; hi = apr_hash_next(hi))
    APR_ARRAY_PUSH(changed_paths, const char *) = svn__apr_hash_index_key(hi);
  qsort(changed_paths->elts, changed_paths->nelts,
        changed_paths->elt_size, svn_sort_compare_paths);

  /* Now, traverse the array of changed paths, verify locks.  Note
     that if we need to do a recursive verification a path, we'll skip
     over children of that path when we get to them. */
  for (i = 0; i < changed_paths->nelts; i++)
    {
      const char *path;
      svn_fs_path_change2_t *change;
      svn_boolean_t recurse = TRUE;

      svn_pool_clear(subpool);
      path = APR_ARRAY_IDX(changed_paths, i, const char *);

      /* If this path has already been verified as part of a recursive
         check of one of its parents, no need to do it again.  */
      if (last_recursed
          && svn_dirent_is_child(last_recursed->data, path, subpool))
        continue;

      /* Fetch the change associated with our path.  */
      change = svn_hash_gets(changes, path);

      /* What does it mean to succeed at lock verification for a given
         path?  For an existing file or directory getting modified
         (text, props), it means we hold the lock on the file or
         directory.  For paths being added or removed, we need to hold
         the locks for that path and any children of that path.

         WHEW!  We have no reliable way to determine the node kind
         of deleted items, but fortunately we are going to do a
         recursive check on deleted paths regardless of their kind.  */
      if (change->change_kind == svn_fs_path_change_modify)
        recurse = FALSE;
      SVN_ERR(svn_fs_fs__allow_locked_operation(path, fs, recurse, TRUE,
                                                subpool));

      /* If we just did a recursive check, remember the path we
         checked (so children can be skipped).  */
      if (recurse)
        {
          if (! last_recursed)
            last_recursed = svn_stringbuf_create(path, pool);
          else
            svn_stringbuf_set(last_recursed, path);
        }
    }
  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* Baton used for commit_body below. */
struct commit_baton {
  svn_revnum_t *new_rev_p;
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  apr_array_header_t *reps_to_cache;
  apr_hash_t *reps_hash;
  apr_pool_t *reps_pool;
};

/* The work-horse for svn_fs_fs__commit, called with the FS write lock.
   This implements the svn_fs_fs__with_write_lock() 'body' callback
   type.  BATON is a 'struct commit_baton *'. */
static svn_error_t *
commit_body(void *baton, apr_pool_t *pool)
{
  struct commit_baton *cb = baton;
  fs_fs_data_t *ffd = cb->fs->fsap_data;
  const char *old_rev_filename, *rev_filename, *proto_filename;
  const char *revprop_filename, *final_revprop;
  const svn_fs_id_t *root_id, *new_root_id;
  const char *start_node_id = NULL, *start_copy_id = NULL;
  svn_revnum_t old_rev, new_rev;
  apr_file_t *proto_file;
  void *proto_file_lockcookie;
  apr_off_t initial_offset, changed_path_offset;
  apr_hash_t *txnprops;
  apr_array_header_t *txnprop_list;
  svn_prop_t prop;
  svn_string_t date;
  svn_stringbuf_t *trailer;

  /* Get the current youngest revision. */
  SVN_ERR(svn_fs_fs__youngest_rev(&old_rev, cb->fs, pool));

  /* Check to make sure this transaction is based off the most recent
     revision. */
  if (cb->txn->base_rev != old_rev)
    return svn_error_create(SVN_ERR_FS_TXN_OUT_OF_DATE, NULL,
                            _("Transaction out of date"));

  /* Locks may have been added (or stolen) between the calling of
     previous svn_fs.h functions and svn_fs_commit_txn(), so we need
     to re-examine every changed-path in the txn and re-verify all
     discovered locks. */
  SVN_ERR(verify_locks(cb->fs, cb->txn->id, pool));

  /* Get the next node_id and copy_id to use. */
  if (ffd->format < SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT)
    SVN_ERR(get_next_revision_ids(&start_node_id, &start_copy_id, cb->fs,
                                  pool));

  /* We are going to be one better than this puny old revision. */
  new_rev = old_rev + 1;

  /* Get a write handle on the proto revision file. */
  SVN_ERR(get_writable_proto_rev(&proto_file, &proto_file_lockcookie,
                                 cb->fs, cb->txn->id, pool));
  SVN_ERR(svn_fs_fs__get_file_offset(&initial_offset, proto_file, pool));

  /* Write out all the node-revisions and directory contents. */
  root_id = svn_fs_fs__id_txn_create("0", "0", cb->txn->id, pool);
  SVN_ERR(write_final_rev(&new_root_id, proto_file, new_rev, cb->fs, root_id,
                          start_node_id, start_copy_id, initial_offset,
                          cb->reps_to_cache, cb->reps_hash, cb->reps_pool,
                          TRUE, pool));

  /* Write the changed-path information. */
  SVN_ERR(write_final_changed_path_info(&changed_path_offset, proto_file,
                                        cb->fs, cb->txn->id, pool));

  /* Write the final line. */
  trailer = svn_fs_fs__unparse_revision_trailer
               (svn_fs_fs__id_offset(new_root_id),
                changed_path_offset,
                pool);
  SVN_ERR(svn_io_file_write_full(proto_file, trailer->data, trailer->len,
                                 NULL, pool));

  SVN_ERR(svn_io_file_flush_to_disk(proto_file, pool));
  SVN_ERR(svn_io_file_close(proto_file, pool));

  /* We don't unlock the prototype revision file immediately to avoid a
     race with another caller writing to the prototype revision file
     before we commit it. */

  /* Remove any temporary txn props representing 'flags'. */
  SVN_ERR(svn_fs_fs__txn_proplist(&txnprops, cb->txn, pool));
  txnprop_list = apr_array_make(pool, 3, sizeof(svn_prop_t));
  prop.value = NULL;

  if (svn_hash_gets(txnprops, SVN_FS__PROP_TXN_CHECK_OOD))
    {
      prop.name = SVN_FS__PROP_TXN_CHECK_OOD;
      APR_ARRAY_PUSH(txnprop_list, svn_prop_t) = prop;
    }

  if (svn_hash_gets(txnprops, SVN_FS__PROP_TXN_CHECK_LOCKS))
    {
      prop.name = SVN_FS__PROP_TXN_CHECK_LOCKS;
      APR_ARRAY_PUSH(txnprop_list, svn_prop_t) = prop;
    }

  if (! apr_is_empty_array(txnprop_list))
    SVN_ERR(svn_fs_fs__change_txn_props(cb->txn, txnprop_list, pool));

  /* Create the shard for the rev and revprop file, if we're sharding and
     this is the first revision of a new shard.  We don't care if this
     fails because the shard already existed for some reason. */
  if (ffd->max_files_per_dir && new_rev % ffd->max_files_per_dir == 0)
    {
      /* Create the revs shard. */
        {
          const char *new_dir
            = svn_fs_fs__path_rev_shard(cb->fs, new_rev, pool);
          svn_error_t *err
            = svn_io_dir_make(new_dir, APR_OS_DEFAULT, pool);
          if (err && !APR_STATUS_IS_EEXIST(err->apr_err))
            return svn_error_trace(err);
          svn_error_clear(err);
          SVN_ERR(svn_io_copy_perms(svn_dirent_join(cb->fs->path,
                                                    PATH_REVS_DIR,
                                                    pool),
                                    new_dir, pool));
        }

      /* Create the revprops shard. */
      SVN_ERR_ASSERT(! svn_fs_fs__is_packed_revprop(cb->fs, new_rev));
        {
          const char *new_dir
            = svn_fs_fs__path_revprops_shard(cb->fs, new_rev, pool);
          svn_error_t *err
            = svn_io_dir_make(new_dir, APR_OS_DEFAULT, pool);
          if (err && !APR_STATUS_IS_EEXIST(err->apr_err))
            return svn_error_trace(err);
          svn_error_clear(err);
          SVN_ERR(svn_io_copy_perms(svn_dirent_join(cb->fs->path,
                                                    PATH_REVPROPS_DIR,
                                                    pool),
                                    new_dir, pool));
        }
    }

  /* Move the finished rev file into place. */
  old_rev_filename = svn_fs_fs__path_rev_absolute(cb->fs, old_rev, pool);
  rev_filename = svn_fs_fs__path_rev(cb->fs, new_rev, pool);
  proto_filename = svn_fs_fs__path_txn_proto_rev(cb->fs, cb->txn->id, pool);
  SVN_ERR(svn_fs_fs__move_into_place(proto_filename, rev_filename,
                                     old_rev_filename, pool));

  /* Now that we've moved the prototype revision file out of the way,
     we can unlock it (since further attempts to write to the file
     will fail as it no longer exists).  We must do this so that we can
     remove the transaction directory later. */
  SVN_ERR(unlock_proto_rev(cb->fs, cb->txn->id, proto_file_lockcookie, pool));

  /* Update commit time to ensure that svn:date revprops remain ordered. */
  date.data = svn_time_to_cstring(apr_time_now(), pool);
  date.len = strlen(date.data);

  SVN_ERR(svn_fs_fs__change_txn_prop(cb->txn, SVN_PROP_REVISION_DATE,
                                     &date, pool));

  /* Move the revprops file into place. */
  SVN_ERR_ASSERT(! svn_fs_fs__is_packed_revprop(cb->fs, new_rev));
  revprop_filename = path_txn_props(cb->fs, cb->txn->id, pool);
  final_revprop = svn_fs_fs__path_revprops(cb->fs, new_rev, pool);
  SVN_ERR(svn_fs_fs__move_into_place(revprop_filename, final_revprop,
                                     old_rev_filename, pool));

  /* Update the 'current' file. */
  SVN_ERR(verify_as_revision_before_current_plus_plus(cb->fs, new_rev, pool));
  SVN_ERR(write_final_current(cb->fs, cb->txn->id, new_rev, start_node_id,
                              start_copy_id, pool));

  /* At this point the new revision is committed and globally visible
     so let the caller know it succeeded by giving it the new revision
     number, which fulfills svn_fs_commit_txn() contract.  Any errors
     after this point do not change the fact that a new revision was
     created. */
  *cb->new_rev_p = new_rev;

  ffd->youngest_rev_cache = new_rev;

  /* Remove this transaction directory. */
  SVN_ERR(svn_fs_fs__purge_txn(cb->fs, cb->txn->id, pool));

  return SVN_NO_ERROR;
}

/* Add the representations in REPS_TO_CACHE (an array of representation_t *)
 * to the rep-cache database of FS. */
static svn_error_t *
write_reps_to_cache(svn_fs_t *fs,
                    const apr_array_header_t *reps_to_cache,
                    apr_pool_t *scratch_pool)
{
  int i;

  for (i = 0; i < reps_to_cache->nelts; i++)
    {
      representation_t *rep = APR_ARRAY_IDX(reps_to_cache, i, representation_t *);

      /* FALSE because we don't care if another parallel commit happened to
       * collide with us.  (Non-parallel collisions will not be detected.) */
      SVN_ERR(svn_fs_fs__set_rep_reference(fs, rep, FALSE, scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__commit(svn_revnum_t *new_rev_p,
                  svn_fs_t *fs,
                  svn_fs_txn_t *txn,
                  apr_pool_t *pool)
{
  struct commit_baton cb;
  fs_fs_data_t *ffd = fs->fsap_data;

  cb.new_rev_p = new_rev_p;
  cb.fs = fs;
  cb.txn = txn;

  if (ffd->rep_sharing_allowed)
    {
      cb.reps_to_cache = apr_array_make(pool, 5, sizeof(representation_t *));
      cb.reps_hash = apr_hash_make(pool);
      cb.reps_pool = pool;
    }
  else
    {
      cb.reps_to_cache = NULL;
      cb.reps_hash = NULL;
      cb.reps_pool = NULL;
    }

  SVN_ERR(svn_fs_fs__with_write_lock(fs, commit_body, &cb, pool));

  /* At this point, *NEW_REV_P has been set, so errors below won't affect
     the success of the commit.  (See svn_fs_commit_txn().)  */

  if (ffd->rep_sharing_allowed)
    {
      SVN_ERR(svn_fs_fs__open_rep_cache(fs, pool));

      /* Write new entries to the rep-sharing database.
       *
       * We use an sqlite transaction to speed things up;
       * see <http://www.sqlite.org/faq.html#q19>.
       */
      /* ### A commit that touches thousands of files will starve other
             (reader/writer) commits for the duration of the below call.
             Maybe write in batches? */
      SVN_SQLITE__WITH_TXN(
        write_reps_to_cache(fs, cb.reps_to_cache, pool),
        ffd->rep_cache_db);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__reserve_copy_id(const char **copy_id_p,
                           svn_fs_t *fs,
                           const char *txn_id,
                           apr_pool_t *pool)
{
  const char *cur_node_id, *cur_copy_id;
  char *copy_id;
  apr_size_t len;

  /* First read in the current next-ids file. */
  SVN_ERR(read_next_ids(&cur_node_id, &cur_copy_id, fs, txn_id, pool));

  copy_id = apr_pcalloc(pool, strlen(cur_copy_id) + 2);

  len = strlen(cur_copy_id);
  svn_fs_fs__next_key(cur_copy_id, &len, copy_id);

  SVN_ERR(write_next_ids(fs, txn_id, cur_node_id, copy_id, pool));

  *copy_id_p = apr_pstrcat(pool, "_", cur_copy_id, (char *)NULL);

  return SVN_NO_ERROR;
}

/* Write out the zeroth revision for filesystem FS. */
static svn_error_t *
write_revision_zero(svn_fs_t *fs)
{
  const char *path_revision_zero = svn_fs_fs__path_rev(fs, 0, fs->pool);
  apr_hash_t *proplist;
  svn_string_t date;

  /* Write out a rev file for revision 0. */
  SVN_ERR(svn_io_file_create(path_revision_zero,
                             "PLAIN\nEND\nENDREP\n"
                             "id: 0.0.r0/17\n"
                             "type: dir\n"
                             "count: 0\n"
                             "text: 0 0 4 4 "
                             "2d2977d1c96f487abe4a1e202dd03b4e\n"
                             "cpath: /\n"
                             "\n\n17 107\n", fs->pool));
  SVN_ERR(svn_io_set_file_read_only(path_revision_zero, FALSE, fs->pool));

  /* Set a date on revision 0. */
  date.data = svn_time_to_cstring(apr_time_now(), fs->pool);
  date.len = strlen(date.data);
  proplist = apr_hash_make(fs->pool);
  svn_hash_sets(proplist, SVN_PROP_REVISION_DATE, &date);
  return svn_fs_fs__set_revision_proplist(fs, 0, proplist, fs->pool);
}

svn_error_t *
svn_fs_fs__create(svn_fs_t *fs,
                  const char *path,
                  apr_pool_t *pool)
{
  int format = SVN_FS_FS__FORMAT_NUMBER;
  fs_fs_data_t *ffd = fs->fsap_data;

  fs->path = apr_pstrdup(pool, path);
  /* See if compatibility with older versions was explicitly requested. */
  if (fs->config)
    {
      if (svn_hash_gets(fs->config, SVN_FS_CONFIG_PRE_1_4_COMPATIBLE))
        format = 1;
      else if (svn_hash_gets(fs->config, SVN_FS_CONFIG_PRE_1_5_COMPATIBLE))
        format = 2;
      else if (svn_hash_gets(fs->config, SVN_FS_CONFIG_PRE_1_6_COMPATIBLE))
        format = 3;
      else if (svn_hash_gets(fs->config, SVN_FS_CONFIG_PRE_1_8_COMPATIBLE))
        format = 4;
    }
  ffd->format = format;

  /* Override the default linear layout if this is a new-enough format. */
  if (format >= SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT)
    ffd->max_files_per_dir = SVN_FS_FS_DEFAULT_MAX_FILES_PER_DIR;

  /* Create the revision data directories. */
  if (ffd->max_files_per_dir)
    SVN_ERR(svn_io_make_dir_recursively(svn_fs_fs__path_rev_shard(fs, 0,
                                                                  pool),
                                        pool));
  else
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(path, PATH_REVS_DIR,
                                                        pool),
                                        pool));

  /* Create the revprops directory. */
  if (ffd->max_files_per_dir)
    SVN_ERR(svn_io_make_dir_recursively(svn_fs_fs__path_revprops_shard(fs, 0,
                                                                       pool),
                                        pool));
  else
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(path,
                                                        PATH_REVPROPS_DIR,
                                                        pool),
                                        pool));

  /* Create the transaction directory. */
  SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(path, PATH_TXNS_DIR,
                                                      pool),
                                      pool));

  /* Create the protorevs directory. */
  if (format >= SVN_FS_FS__MIN_PROTOREVS_DIR_FORMAT)
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(path, PATH_TXN_PROTOS_DIR,
                                                      pool),
                                        pool));

  /* Create the 'current' file. */
  SVN_ERR(svn_io_file_create(svn_fs_fs__path_current(fs, pool),
                             (format >= SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT
                              ? "0\n" : "0 1 1\n"),
                             pool));
  SVN_ERR(svn_io_file_create(path_lock(fs, pool), "", pool));
  SVN_ERR(svn_fs_fs__set_uuid(fs, NULL, pool));

  SVN_ERR(write_revision_zero(fs));

  SVN_ERR(write_config(fs, pool));

  SVN_ERR(read_config(ffd, fs->path, pool));

  /* Create the min unpacked rev file. */
  if (ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
    SVN_ERR(svn_io_file_create(svn_fs_fs__path_min_unpacked_rev(fs, pool),
                               "0\n", pool));

  /* Create the txn-current file if the repository supports
     the transaction sequence file. */
  if (format >= SVN_FS_FS__MIN_TXN_CURRENT_FORMAT)
    {
      SVN_ERR(svn_io_file_create(path_txn_current(fs, pool),
                                 "0\n", pool));
      SVN_ERR(svn_io_file_create(path_txn_current_lock(fs, pool),
                                 "", pool));
    }

  /* This filesystem is ready.  Stamp it with a format number. */
  SVN_ERR(write_format(path_format(fs, pool),
                       ffd->format, ffd->max_files_per_dir, FALSE, pool));

  ffd->youngest_rev_cache = 0;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__set_uuid(svn_fs_t *fs,
                    const char *uuid,
                    apr_pool_t *pool)
{
  char *my_uuid;
  apr_size_t my_uuid_len;
  const char *uuid_path = path_uuid(fs, pool);

  if (! uuid)
    uuid = svn_uuid_generate(pool);

  /* Make sure we have a copy in FS->POOL, and append a newline. */
  my_uuid = apr_pstrcat(fs->pool, uuid, "\n", (char *)NULL);
  my_uuid_len = strlen(my_uuid);

  /* We use the permissions of the 'current' file, because the 'uuid'
     file does not exist during repository creation. */
  SVN_ERR(svn_io_write_atomic(uuid_path, my_uuid, my_uuid_len,
                              svn_fs_fs__path_current(fs, pool) /* perms */,
                              pool));

  /* Remove the newline we added, and stash the UUID. */
  my_uuid[my_uuid_len - 1] = '\0';
  fs->uuid = my_uuid;

  return SVN_NO_ERROR;
}

/** Node origin lazy cache. */

/* If directory PATH does not exist, create it and give it the same
   permissions as FS_path.*/
svn_error_t *
svn_fs_fs__ensure_dir_exists(const char *path,
                             const char *fs_path,
                             apr_pool_t *pool)
{
  svn_error_t *err = svn_io_dir_make(path, APR_OS_DEFAULT, pool);
  if (err && APR_STATUS_IS_EEXIST(err->apr_err))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  /* We successfully created a new directory.  Dup the permissions
     from FS->path. */
  return svn_io_copy_perms(fs_path, path, pool);
}

/* Set *NODE_ORIGINS to a hash mapping 'const char *' node IDs to
   'svn_string_t *' node revision IDs.  Use POOL for allocations. */
static svn_error_t *
get_node_origins_from_file(svn_fs_t *fs,
                           apr_hash_t **node_origins,
                           const char *node_origins_file,
                           apr_pool_t *pool)
{
  apr_file_t *fd;
  svn_error_t *err;
  svn_stream_t *stream;

  *node_origins = NULL;
  err = svn_io_file_open(&fd, node_origins_file,
                         APR_READ, APR_OS_DEFAULT, pool);
  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  stream = svn_stream_from_aprfile2(fd, FALSE, pool);
  *node_origins = apr_hash_make(pool);
  SVN_ERR(svn_hash_read2(*node_origins, stream, SVN_HASH_TERMINATOR, pool));
  return svn_stream_close(stream);
}

svn_error_t *
svn_fs_fs__get_node_origin(const svn_fs_id_t **origin_id,
                           svn_fs_t *fs,
                           const char *node_id,
                           apr_pool_t *pool)
{
  apr_hash_t *node_origins;

  *origin_id = NULL;
  SVN_ERR(get_node_origins_from_file(fs, &node_origins,
                                     path_node_origin(fs, node_id, pool),
                                     pool));
  if (node_origins)
    {
      svn_string_t *origin_id_str =
        svn_hash_gets(node_origins, node_id);
      if (origin_id_str)
        *origin_id = svn_fs_fs__id_parse(origin_id_str->data,
                                         origin_id_str->len, pool);
    }
  return SVN_NO_ERROR;
}


/* Helper for svn_fs_fs__set_node_origin.  Takes a NODE_ID/NODE_REV_ID
   pair and adds it to the NODE_ORIGINS_PATH file.  */
static svn_error_t *
set_node_origins_for_file(svn_fs_t *fs,
                          const char *node_origins_path,
                          const char *node_id,
                          svn_string_t *node_rev_id,
                          apr_pool_t *pool)
{
  const char *path_tmp;
  svn_stream_t *stream;
  apr_hash_t *origins_hash;
  svn_string_t *old_node_rev_id;

  SVN_ERR(svn_fs_fs__ensure_dir_exists(svn_dirent_join(fs->path,
                                                       PATH_NODE_ORIGINS_DIR,
                                                       pool),
                                       fs->path, pool));

  /* Read the previously existing origins (if any), and merge our
     update with it. */
  SVN_ERR(get_node_origins_from_file(fs, &origins_hash,
                                     node_origins_path, pool));
  if (! origins_hash)
    origins_hash = apr_hash_make(pool);

  old_node_rev_id = svn_hash_gets(origins_hash, node_id);

  if (old_node_rev_id && !svn_string_compare(node_rev_id, old_node_rev_id))
    return svn_error_createf(SVN_ERR_FS_CORRUPT, NULL,
                             _("Node origin for '%s' exists with a different "
                               "value (%s) than what we were about to store "
                               "(%s)"),
                             node_id, old_node_rev_id->data, node_rev_id->data);

  svn_hash_sets(origins_hash, node_id, node_rev_id);

  /* Sure, there's a race condition here.  Two processes could be
     trying to add different cache elements to the same file at the
     same time, and the entries added by the first one to write will
     be lost.  But this is just a cache of reconstructible data, so
     we'll accept this problem in return for not having to deal with
     locking overhead. */

  /* Create a temporary file, write out our hash, and close the file. */
  SVN_ERR(svn_stream_open_unique(&stream, &path_tmp,
                                 svn_dirent_dirname(node_origins_path, pool),
                                 svn_io_file_del_none, pool, pool));
  SVN_ERR(svn_hash_write2(origins_hash, stream, SVN_HASH_TERMINATOR, pool));
  SVN_ERR(svn_stream_close(stream));

  /* Rename the temp file as the real destination */
  return svn_io_file_rename(path_tmp, node_origins_path, pool);
}


svn_error_t *
svn_fs_fs__set_node_origin(svn_fs_t *fs,
                           const char *node_id,
                           const svn_fs_id_t *node_rev_id,
                           apr_pool_t *pool)
{
  svn_error_t *err;
  const char *filename = path_node_origin(fs, node_id, pool);

  err = set_node_origins_for_file(fs, filename,
                                  node_id,
                                  svn_fs_fs__id_unparse(node_rev_id, pool),
                                  pool);
  if (err && APR_STATUS_IS_EACCES(err->apr_err))
    {
      /* It's just a cache; stop trying if I can't write. */
      svn_error_clear(err);
      err = NULL;
    }
  return svn_error_trace(err);
}


svn_error_t *
svn_fs_fs__list_transactions(apr_array_header_t **names_p,
                             svn_fs_t *fs,
                             apr_pool_t *pool)
{
  const char *txn_dir;
  apr_hash_t *dirents;
  apr_hash_index_t *hi;
  apr_array_header_t *names;
  apr_size_t ext_len = strlen(PATH_EXT_TXN);

  names = apr_array_make(pool, 1, sizeof(const char *));

  /* Get the transactions directory. */
  txn_dir = svn_dirent_join(fs->path, PATH_TXNS_DIR, pool);

  /* Now find a listing of this directory. */
  SVN_ERR(svn_io_get_dirents3(&dirents, txn_dir, TRUE, pool, pool));

  /* Loop through all the entries and return anything that ends with '.txn'. */
  for (hi = apr_hash_first(pool, dirents); hi; hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);
      apr_ssize_t klen = svn__apr_hash_index_klen(hi);
      const char *id;

      /* The name must end with ".txn" to be considered a transaction. */
      if ((apr_size_t) klen <= ext_len
          || (strcmp(name + klen - ext_len, PATH_EXT_TXN)) != 0)
        continue;

      /* Truncate the ".txn" extension and store the ID. */
      id = apr_pstrndup(pool, name, strlen(name) - ext_len);
      APR_ARRAY_PUSH(names, const char *) = id;
    }

  *names_p = names;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__open_txn(svn_fs_txn_t **txn_p,
                    svn_fs_t *fs,
                    const char *name,
                    apr_pool_t *pool)
{
  svn_fs_txn_t *txn;
  svn_node_kind_t kind;
  transaction_t *local_txn;

  /* First check to see if the directory exists. */
  SVN_ERR(svn_io_check_path(svn_fs_fs__path_txn_dir(fs, name, pool), &kind,
                            pool));

  /* Did we find it? */
  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_TRANSACTION, NULL,
                             _("No such transaction '%s'"),
                             name);

  txn = apr_pcalloc(pool, sizeof(*txn));

  /* Read in the root node of this transaction. */
  txn->id = apr_pstrdup(pool, name);
  txn->fs = fs;

  SVN_ERR(svn_fs_fs__get_txn(&local_txn, fs, name, pool));

  txn->base_rev = svn_fs_fs__id_rev(local_txn->base_id);

  txn->vtable = &txn_vtable;
  *txn_p = txn;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__txn_proplist(apr_hash_t **table_p,
                        svn_fs_txn_t *txn,
                        apr_pool_t *pool)
{
  apr_hash_t *proplist = apr_hash_make(pool);
  SVN_ERR(get_txn_proplist(proplist, txn->fs, txn->id, pool));
  *table_p = proplist;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__delete_node_revision(svn_fs_t *fs,
                                const svn_fs_id_t *id,
                                apr_pool_t *pool)
{
  node_revision_t *noderev;

  SVN_ERR(svn_fs_fs__get_node_revision(&noderev, fs, id, pool));

  /* Delete any mutable property representation. */
  if (noderev->prop_rep && noderev->prop_rep->txn_id)
    SVN_ERR(svn_io_remove_file2(svn_fs_fs__path_txn_node_props(fs, id, pool),
                                FALSE, pool));

  /* Delete any mutable data representation. */
  if (noderev->data_rep && noderev->data_rep->txn_id
      && noderev->kind == svn_node_dir)
    {
      fs_fs_data_t *ffd = fs->fsap_data;
      SVN_ERR(svn_io_remove_file2(svn_fs_fs__path_txn_node_children(fs, id,
                                                                    pool),
                                  FALSE, pool));

      /* remove the corresponding entry from the cache, if such exists */
      if (ffd->txn_dir_cache)
        {
          const char *key = svn_fs_fs__id_unparse(id, pool)->data;
          SVN_ERR(svn_cache__set(ffd->txn_dir_cache, key, NULL, pool));
        }
    }

  return svn_io_remove_file2(svn_fs_fs__path_txn_node_rev(fs, id, pool),
                             FALSE, pool);
}



/*** Revisions ***/

svn_error_t *
svn_fs_fs__revision_prop(svn_string_t **value_p,
                         svn_fs_t *fs,
                         svn_revnum_t rev,
                         const char *propname,
                         apr_pool_t *pool)
{
  apr_hash_t *table;

  SVN_ERR(svn_fs__check_fs(fs, TRUE));
  SVN_ERR(svn_fs_fs__get_revision_proplist(&table, fs, rev, pool));

  *value_p = svn_hash_gets(table, propname);

  return SVN_NO_ERROR;
}


/* Baton used for change_rev_prop_body below. */
struct change_rev_prop_baton {
  svn_fs_t *fs;
  svn_revnum_t rev;
  const char *name;
  const svn_string_t *const *old_value_p;
  const svn_string_t *value;
};

/* The work-horse for svn_fs_fs__change_rev_prop, called with the FS
   write lock.  This implements the svn_fs_fs__with_write_lock()
   'body' callback type.  BATON is a 'struct change_rev_prop_baton *'. */
static svn_error_t *
change_rev_prop_body(void *baton, apr_pool_t *pool)
{
  struct change_rev_prop_baton *cb = baton;
  apr_hash_t *table;

  SVN_ERR(svn_fs_fs__get_revision_proplist(&table, cb->fs, cb->rev, pool));

  if (cb->old_value_p)
    {
      const svn_string_t *wanted_value = *cb->old_value_p;
      const svn_string_t *present_value = svn_hash_gets(table, cb->name);
      if ((!wanted_value != !present_value)
          || (wanted_value && present_value
              && !svn_string_compare(wanted_value, present_value)))
        {
          /* What we expected isn't what we found. */
          return svn_error_createf(SVN_ERR_FS_PROP_BASEVALUE_MISMATCH, NULL,
                                   _("revprop '%s' has unexpected value in "
                                     "filesystem"),
                                   cb->name);
        }
      /* Fall through. */
    }
  svn_hash_sets(table, cb->name, cb->value);

  return svn_fs_fs__set_revision_proplist(cb->fs, cb->rev, table, pool);
}

svn_error_t *
svn_fs_fs__change_rev_prop(svn_fs_t *fs,
                           svn_revnum_t rev,
                           const char *name,
                           const svn_string_t *const *old_value_p,
                           const svn_string_t *value,
                           apr_pool_t *pool)
{
  struct change_rev_prop_baton cb;

  SVN_ERR(svn_fs__check_fs(fs, TRUE));

  cb.fs = fs;
  cb.rev = rev;
  cb.name = name;
  cb.old_value_p = old_value_p;
  cb.value = value;

  return svn_fs_fs__with_write_lock(fs, change_rev_prop_body, &cb, pool);
}



/*** Transactions ***/

svn_error_t *
svn_fs_fs__get_txn_ids(const svn_fs_id_t **root_id_p,
                       const svn_fs_id_t **base_root_id_p,
                       svn_fs_t *fs,
                       const char *txn_name,
                       apr_pool_t *pool)
{
  transaction_t *txn;
  SVN_ERR(svn_fs_fs__get_txn(&txn, fs, txn_name, pool));
  *root_id_p = txn->root_id;
  *base_root_id_p = txn->base_id;
  return SVN_NO_ERROR;
}


/* Generic transaction operations.  */

svn_error_t *
svn_fs_fs__txn_prop(svn_string_t **value_p,
                    svn_fs_txn_t *txn,
                    const char *propname,
                    apr_pool_t *pool)
{
  apr_hash_t *table;
  svn_fs_t *fs = txn->fs;

  SVN_ERR(svn_fs__check_fs(fs, TRUE));
  SVN_ERR(svn_fs_fs__txn_proplist(&table, txn, pool));

  *value_p = svn_hash_gets(table, propname);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__begin_txn(svn_fs_txn_t **txn_p,
                     svn_fs_t *fs,
                     svn_revnum_t rev,
                     apr_uint32_t flags,
                     apr_pool_t *pool)
{
  svn_string_t date;
  svn_prop_t prop;
  apr_array_header_t *props = apr_array_make(pool, 3, sizeof(svn_prop_t));

  SVN_ERR(svn_fs__check_fs(fs, TRUE));

  SVN_ERR(svn_fs_fs__create_txn(txn_p, fs, rev, pool));

  /* Put a datestamp on the newly created txn, so we always know
     exactly how old it is.  (This will help sysadmins identify
     long-abandoned txns that may need to be manually removed.)  When
     a txn is promoted to a revision, this property will be
     automatically overwritten with a revision datestamp. */
  date.data = svn_time_to_cstring(apr_time_now(), pool);
  date.len = strlen(date.data);

  prop.name = SVN_PROP_REVISION_DATE;
  prop.value = &date;
  APR_ARRAY_PUSH(props, svn_prop_t) = prop;

  /* Set temporary txn props that represent the requested 'flags'
     behaviors. */
  if (flags & SVN_FS_TXN_CHECK_OOD)
    {
      prop.name = SVN_FS__PROP_TXN_CHECK_OOD;
      prop.value = svn_string_create("true", pool);
      APR_ARRAY_PUSH(props, svn_prop_t) = prop;
    }

  if (flags & SVN_FS_TXN_CHECK_LOCKS)
    {
      prop.name = SVN_FS__PROP_TXN_CHECK_LOCKS;
      prop.value = svn_string_create("true", pool);
      APR_ARRAY_PUSH(props, svn_prop_t) = prop;
    }

  return svn_fs_fs__change_txn_props(*txn_p, props, pool);
}


/** Hotcopy. **/

/* Like svn_io_dir_file_copy(), but doesn't copy files that exist at
 * the destination and do not differ in terms of kind, size, and mtime. */
static svn_error_t *
hotcopy_io_dir_file_copy(const char *src_path,
                         const char *dst_path,
                         const char *file,
                         apr_pool_t *scratch_pool)
{
  const svn_io_dirent2_t *src_dirent;
  const svn_io_dirent2_t *dst_dirent;
  const char *src_target;
  const char *dst_target;

  /* Does the destination already exist? If not, we must copy it. */
  dst_target = svn_dirent_join(dst_path, file, scratch_pool);
  SVN_ERR(svn_io_stat_dirent2(&dst_dirent, dst_target, FALSE, TRUE,
                              scratch_pool, scratch_pool));
  if (dst_dirent->kind != svn_node_none)
    {
      /* If the destination's stat information indicates that the file
       * is equal to the source, don't bother copying the file again. */
      src_target = svn_dirent_join(src_path, file, scratch_pool);
      SVN_ERR(svn_io_stat_dirent2(&src_dirent, src_target, FALSE, FALSE,
                                  scratch_pool, scratch_pool));
      if (src_dirent->kind == dst_dirent->kind &&
          src_dirent->special == dst_dirent->special &&
          src_dirent->filesize == dst_dirent->filesize &&
          src_dirent->mtime <= dst_dirent->mtime)
        return SVN_NO_ERROR;
    }

  return svn_error_trace(svn_io_dir_file_copy(src_path, dst_path, file,
                                              scratch_pool));
}

/* Set *NAME_P to the UTF-8 representation of directory entry NAME.
 * NAME is in the internal encoding used by APR; PARENT is in
 * UTF-8 and in internal (not local) style.
 *
 * Use PARENT only for generating an error string if the conversion
 * fails because NAME could not be represented in UTF-8.  In that
 * case, return a two-level error in which the outer error's message
 * mentions PARENT, but the inner error's message does not mention
 * NAME (except possibly in hex) since NAME may not be printable.
 * Such a compound error at least allows the user to go looking in the
 * right directory for the problem.
 *
 * If there is any other error, just return that error directly.
 *
 * If there is any error, the effect on *NAME_P is undefined.
 *
 * *NAME_P and NAME may refer to the same storage.
 */
static svn_error_t *
entry_name_to_utf8(const char **name_p,
                   const char *name,
                   const char *parent,
                   apr_pool_t *pool)
{
  svn_error_t *err = svn_path_cstring_to_utf8(name_p, name, pool);
  if (err && err->apr_err == APR_EINVAL)
    {
      return svn_error_createf(err->apr_err, err,
                               _("Error converting entry "
                                 "in directory '%s' to UTF-8"),
                               svn_dirent_local_style(parent, pool));
    }
  return err;
}

/* Like svn_io_copy_dir_recursively() but doesn't copy regular files that
 * exist in the destination and do not differ from the source in terms of
 * kind, size, and mtime. */
static svn_error_t *
hotcopy_io_copy_dir_recursively(const char *src,
                                const char *dst_parent,
                                const char *dst_basename,
                                svn_boolean_t copy_perms,
                                svn_cancel_func_t cancel_func,
                                void *cancel_baton,
                                apr_pool_t *pool)
{
  svn_node_kind_t kind;
  apr_status_t status;
  const char *dst_path;
  apr_dir_t *this_dir;
  apr_finfo_t this_entry;
  apr_int32_t flags = APR_FINFO_TYPE | APR_FINFO_NAME;

  /* Make a subpool for recursion */
  apr_pool_t *subpool = svn_pool_create(pool);

  /* The 'dst_path' is simply dst_parent/dst_basename */
  dst_path = svn_dirent_join(dst_parent, dst_basename, pool);

  /* Sanity checks:  SRC and DST_PARENT are directories, and
     DST_BASENAME doesn't already exist in DST_PARENT. */
  SVN_ERR(svn_io_check_path(src, &kind, subpool));
  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                             _("Source '%s' is not a directory"),
                             svn_dirent_local_style(src, pool));

  SVN_ERR(svn_io_check_path(dst_parent, &kind, subpool));
  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                             _("Destination '%s' is not a directory"),
                             svn_dirent_local_style(dst_parent, pool));

  SVN_ERR(svn_io_check_path(dst_path, &kind, subpool));

  /* Create the new directory. */
  /* ### TODO: copy permissions (needs apr_file_attrs_get()) */
  SVN_ERR(svn_io_make_dir_recursively(dst_path, pool));

  /* Loop over the dirents in SRC.  ('.' and '..' are auto-excluded) */
  SVN_ERR(svn_io_dir_open(&this_dir, src, subpool));

  for (status = apr_dir_read(&this_entry, flags, this_dir);
       status == APR_SUCCESS;
       status = apr_dir_read(&this_entry, flags, this_dir))
    {
      if ((this_entry.name[0] == '.')
          && ((this_entry.name[1] == '\0')
              || ((this_entry.name[1] == '.')
                  && (this_entry.name[2] == '\0'))))
        {
          continue;
        }
      else
        {
          const char *entryname_utf8;

          if (cancel_func)
            SVN_ERR(cancel_func(cancel_baton));

          SVN_ERR(entry_name_to_utf8(&entryname_utf8, this_entry.name,
                                     src, subpool));
          if (this_entry.filetype == APR_REG) /* regular file */
            {
              SVN_ERR(hotcopy_io_dir_file_copy(src, dst_path, entryname_utf8,
                                               subpool));
            }
          else if (this_entry.filetype == APR_LNK) /* symlink */
            {
              const char *src_target = svn_dirent_join(src, entryname_utf8,
                                                       subpool);
              const char *dst_target = svn_dirent_join(dst_path,
                                                       entryname_utf8,
                                                       subpool);
              SVN_ERR(svn_io_copy_link(src_target, dst_target,
                                       subpool));
            }
          else if (this_entry.filetype == APR_DIR) /* recurse */
            {
              const char *src_target;

              /* Prevent infinite recursion by filtering off our
                 newly created destination path. */
              if (strcmp(src, dst_parent) == 0
                  && strcmp(entryname_utf8, dst_basename) == 0)
                continue;

              src_target = svn_dirent_join(src, entryname_utf8, subpool);
              SVN_ERR(hotcopy_io_copy_dir_recursively(src_target,
                                                      dst_path,
                                                      entryname_utf8,
                                                      copy_perms,
                                                      cancel_func,
                                                      cancel_baton,
                                                      subpool));
            }
          /* ### support other APR node types someday?? */

        }
    }

  if (! (APR_STATUS_IS_ENOENT(status)))
    return svn_error_wrap_apr(status, _("Can't read directory '%s'"),
                              svn_dirent_local_style(src, pool));

  status = apr_dir_close(this_dir);
  if (status)
    return svn_error_wrap_apr(status, _("Error closing directory '%s'"),
                              svn_dirent_local_style(src, pool));

  /* Free any memory used by recursion */
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Copy an un-packed revision or revprop file for revision REV from SRC_SUBDIR
 * to DST_SUBDIR. Assume a sharding layout based on MAX_FILES_PER_DIR.
 * Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
hotcopy_copy_shard_file(const char *src_subdir,
                        const char *dst_subdir,
                        svn_revnum_t rev,
                        int max_files_per_dir,
                        apr_pool_t *scratch_pool)
{
  const char *src_subdir_shard = src_subdir,
             *dst_subdir_shard = dst_subdir;

  if (max_files_per_dir)
    {
      const char *shard = apr_psprintf(scratch_pool, "%ld",
                                       rev / max_files_per_dir);
      src_subdir_shard = svn_dirent_join(src_subdir, shard, scratch_pool);
      dst_subdir_shard = svn_dirent_join(dst_subdir, shard, scratch_pool);

      if (rev % max_files_per_dir == 0)
        {
          SVN_ERR(svn_io_make_dir_recursively(dst_subdir_shard, scratch_pool));
          SVN_ERR(svn_io_copy_perms(dst_subdir, dst_subdir_shard,
                                    scratch_pool));
        }
    }

  SVN_ERR(hotcopy_io_dir_file_copy(src_subdir_shard, dst_subdir_shard,
                                   apr_psprintf(scratch_pool, "%ld", rev),
                                   scratch_pool));
  return SVN_NO_ERROR;
}


/* Copy a packed shard containing revision REV, and which contains
 * MAX_FILES_PER_DIR revisions, from SRC_FS to DST_FS.
 * Update *DST_MIN_UNPACKED_REV in case the shard is new in DST_FS.
 * Do not re-copy data which already exists in DST_FS.
 * Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
hotcopy_copy_packed_shard(svn_revnum_t *dst_min_unpacked_rev,
                          svn_fs_t *src_fs,
                          svn_fs_t *dst_fs,
                          svn_revnum_t rev,
                          int max_files_per_dir,
                          apr_pool_t *scratch_pool)
{
  const char *src_subdir;
  const char *dst_subdir;
  const char *packed_shard;
  const char *src_subdir_packed_shard;
  svn_revnum_t revprop_rev;
  apr_pool_t *iterpool;
  fs_fs_data_t *src_ffd = src_fs->fsap_data;

  /* Copy the packed shard. */
  src_subdir = svn_dirent_join(src_fs->path, PATH_REVS_DIR, scratch_pool);
  dst_subdir = svn_dirent_join(dst_fs->path, PATH_REVS_DIR, scratch_pool);
  packed_shard = apr_psprintf(scratch_pool, "%ld" PATH_EXT_PACKED_SHARD,
                              rev / max_files_per_dir);
  src_subdir_packed_shard = svn_dirent_join(src_subdir, packed_shard,
                                            scratch_pool);
  SVN_ERR(hotcopy_io_copy_dir_recursively(src_subdir_packed_shard,
                                          dst_subdir, packed_shard,
                                          TRUE /* copy_perms */,
                                          NULL /* cancel_func */, NULL,
                                          scratch_pool));

  /* Copy revprops belonging to revisions in this pack. */
  src_subdir = svn_dirent_join(src_fs->path, PATH_REVPROPS_DIR, scratch_pool);
  dst_subdir = svn_dirent_join(dst_fs->path, PATH_REVPROPS_DIR, scratch_pool);

  if (   src_ffd->format < SVN_FS_FS__MIN_PACKED_REVPROP_FORMAT
      || src_ffd->min_unpacked_rev < rev + max_files_per_dir)
    {
      /* copy unpacked revprops rev by rev */
      iterpool = svn_pool_create(scratch_pool);
      for (revprop_rev = rev;
           revprop_rev < rev + max_files_per_dir;
           revprop_rev++)
        {
          svn_pool_clear(iterpool);

          SVN_ERR(hotcopy_copy_shard_file(src_subdir, dst_subdir,
                                          revprop_rev, max_files_per_dir,
                                          iterpool));
        }
      svn_pool_destroy(iterpool);
    }
  else
    {
      /* revprop for revision 0 will never be packed */
      if (rev == 0)
        SVN_ERR(hotcopy_copy_shard_file(src_subdir, dst_subdir,
                                        0, max_files_per_dir,
                                        scratch_pool));

      /* packed revprops folder */
      packed_shard = apr_psprintf(scratch_pool, "%ld" PATH_EXT_PACKED_SHARD,
                                  rev / max_files_per_dir);
      src_subdir_packed_shard = svn_dirent_join(src_subdir, packed_shard,
                                                scratch_pool);
      SVN_ERR(hotcopy_io_copy_dir_recursively(src_subdir_packed_shard,
                                              dst_subdir, packed_shard,
                                              TRUE /* copy_perms */,
                                              NULL /* cancel_func */, NULL,
                                              scratch_pool));
    }

  /* If necessary, update the min-unpacked rev file in the hotcopy. */
  if (*dst_min_unpacked_rev < rev + max_files_per_dir)
    {
      *dst_min_unpacked_rev = rev + max_files_per_dir;
      SVN_ERR(svn_fs_fs__write_revnum_file(dst_fs, *dst_min_unpacked_rev,
                                           scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* If NEW_YOUNGEST is younger than *DST_YOUNGEST, update the 'current'
 * file in DST_FS and set *DST_YOUNGEST to NEW_YOUNGEST.
 * Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
hotcopy_update_current(svn_revnum_t *dst_youngest,
                       svn_fs_t *dst_fs,
                       svn_revnum_t new_youngest,
                       apr_pool_t *scratch_pool)
{
  char next_node_id[MAX_KEY_SIZE] = "0";
  char next_copy_id[MAX_KEY_SIZE] = "0";
  fs_fs_data_t *dst_ffd = dst_fs->fsap_data;

  if (*dst_youngest >= new_youngest)
    return SVN_NO_ERROR;

  /* If necessary, get new current next_node and next_copy IDs. */
  if (dst_ffd->format < SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT)
    SVN_ERR(svn_fs_fs__find_max_ids(dst_fs, new_youngest,
                                    next_node_id, next_copy_id,
                                    scratch_pool));

  /* Update 'current'. */
  SVN_ERR(svn_fs_fs__write_current(dst_fs, new_youngest, next_node_id,
                                   next_copy_id, scratch_pool));

  *dst_youngest = new_youngest;

  return SVN_NO_ERROR;
}


/* Remove revisions between START_REV (inclusive) and END_REV (non-inclusive)
 * from DST_FS. Assume sharding as per MAX_FILES_PER_DIR.
 * Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
hotcopy_remove_rev_files(svn_fs_t *dst_fs,
                         svn_revnum_t start_rev,
                         svn_revnum_t end_rev,
                         int max_files_per_dir,
                         apr_pool_t *scratch_pool)
{
  const char *dst_subdir;
  const char *shard;
  const char *dst_subdir_shard;
  svn_revnum_t rev;
  apr_pool_t *iterpool;

  SVN_ERR_ASSERT(start_rev <= end_rev);

  dst_subdir = svn_dirent_join(dst_fs->path, PATH_REVS_DIR, scratch_pool);

  /* Pre-compute paths for initial shard. */
  shard = apr_psprintf(scratch_pool, "%ld", start_rev / max_files_per_dir);
  dst_subdir_shard = svn_dirent_join(dst_subdir, shard, scratch_pool);

  iterpool = svn_pool_create(scratch_pool);
  for (rev = start_rev; rev < end_rev; rev++)
    {
      const char *rev_path;

      svn_pool_clear(iterpool);

      /* If necessary, update paths for shard. */
      if (rev != start_rev && rev % max_files_per_dir == 0)
        {
          shard = apr_psprintf(iterpool, "%ld", rev / max_files_per_dir);
          dst_subdir_shard = svn_dirent_join(dst_subdir, shard, scratch_pool);
        }

      rev_path = svn_dirent_join(dst_subdir_shard,
                                 apr_psprintf(iterpool, "%ld", rev),
                                 iterpool);

      /* Make the rev file writable and remove it. */
      SVN_ERR(svn_io_set_file_read_write(rev_path, TRUE, iterpool));
      SVN_ERR(svn_io_remove_file2(rev_path, TRUE, iterpool));
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Verify that DST_FS is a suitable destination for an incremental
 * hotcopy from SRC_FS. */
static svn_error_t *
hotcopy_incremental_check_preconditions(svn_fs_t *src_fs,
                                        svn_fs_t *dst_fs,
                                        apr_pool_t *pool)
{
  fs_fs_data_t *src_ffd = src_fs->fsap_data;
  fs_fs_data_t *dst_ffd = dst_fs->fsap_data;

  /* We only support incremental hotcopy between the same format. */
  if (src_ffd->format != dst_ffd->format)
    return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
      _("The FSFS format (%d) of the hotcopy source does not match the "
        "FSFS format (%d) of the hotcopy destination; please upgrade "
        "both repositories to the same format"),
      src_ffd->format, dst_ffd->format);

  /* Make sure the UUID of source and destination match up.
   * We don't want to copy over a different repository. */
  if (strcmp(src_fs->uuid, dst_fs->uuid) != 0)
    return svn_error_create(SVN_ERR_RA_UUID_MISMATCH, NULL,
                            _("The UUID of the hotcopy source does "
                              "not match the UUID of the hotcopy "
                              "destination"));

  /* Also require same shard size. */
  if (src_ffd->max_files_per_dir != dst_ffd->max_files_per_dir)
    return svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                            _("The sharding layout configuration "
                              "of the hotcopy source does not match "
                              "the sharding layout configuration of "
                              "the hotcopy destination"));
  return SVN_NO_ERROR;
}


/* Baton for hotcopy_body(). */
struct hotcopy_body_baton {
  svn_fs_t *src_fs;
  svn_fs_t *dst_fs;
  svn_boolean_t incremental;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;
} hotcopy_body_baton;

/* Perform a hotcopy, either normal or incremental.
 *
 * Normal hotcopy assumes that the destination exists as an empty
 * directory. It behaves like an incremental hotcopy except that
 * none of the copied files already exist in the destination.
 *
 * An incremental hotcopy copies only changed or new files to the destination,
 * and removes files from the destination no longer present in the source.
 * While the incremental hotcopy is running, readers should still be able
 * to access the destintation repository without error and should not see
 * revisions currently in progress of being copied. Readers are able to see
 * new fully copied revisions even if the entire incremental hotcopy procedure
 * has not yet completed.
 *
 * Writers are blocked out completely during the entire incremental hotcopy
 * process to ensure consistency. This function assumes that the repository
 * write-lock is held.
 */
static svn_error_t *
hotcopy_body(void *baton, apr_pool_t *pool)
{
  struct hotcopy_body_baton *hbb = baton;
  svn_fs_t *src_fs = hbb->src_fs;
  fs_fs_data_t *src_ffd = src_fs->fsap_data;
  svn_fs_t *dst_fs = hbb->dst_fs;
  fs_fs_data_t *dst_ffd = dst_fs->fsap_data;
  int max_files_per_dir = src_ffd->max_files_per_dir;
  svn_boolean_t incremental = hbb->incremental;
  svn_cancel_func_t cancel_func = hbb->cancel_func;
  void* cancel_baton = hbb->cancel_baton;
  svn_revnum_t src_youngest;
  svn_revnum_t dst_youngest;
  svn_revnum_t rev;
  svn_revnum_t src_min_unpacked_rev;
  svn_revnum_t dst_min_unpacked_rev;
  const char *src_subdir;
  const char *dst_subdir;
  const char *revprop_src_subdir;
  const char *revprop_dst_subdir;
  apr_pool_t *iterpool;
  svn_node_kind_t kind;

  /* Try to copy the config.
   *
   * ### We try copying the config file before doing anything else,
   * ### because higher layers will abort the hotcopy if we throw
   * ### an error from this function, and that renders the hotcopy
   * ### unusable anyway. */
  if (src_ffd->format >= SVN_FS_FS__MIN_CONFIG_FILE)
    {
      svn_error_t *err;

      err = svn_io_dir_file_copy(src_fs->path, dst_fs->path, PATH_CONFIG,
                                 pool);
      if (err)
        {
          if (APR_STATUS_IS_ENOENT(err->apr_err))
            {
              /* 1.6.0 to 1.6.11 did not copy the configuration file during
               * hotcopy. So if we're hotcopying a repository which has been
               * created as a hotcopy itself, it's possible that fsfs.conf
               * does not exist. Ask the user to re-create it.
               *
               * ### It would be nice to make this a non-fatal error,
               * ### but this function does not get an svn_fs_t object
               * ### so we have no way of just printing a warning via
               * ### the fs->warning() callback. */

              const char *msg;
              const char *src_abspath;
              const char *dst_abspath;
              const char *config_relpath;
              svn_error_t *err2;

              config_relpath = svn_dirent_join(src_fs->path, PATH_CONFIG, pool);
              err2 = svn_dirent_get_absolute(&src_abspath, src_fs->path, pool);
              if (err2)
                return svn_error_trace(svn_error_compose_create(err, err2));
              err2 = svn_dirent_get_absolute(&dst_abspath, dst_fs->path, pool);
              if (err2)
                return svn_error_trace(svn_error_compose_create(err, err2));

              /* ### hack: strip off the 'db/' directory from paths so
               * ### they make sense to the user */
              src_abspath = svn_dirent_dirname(src_abspath, pool);
              dst_abspath = svn_dirent_dirname(dst_abspath, pool);

              msg = apr_psprintf(pool,
                                 _("Failed to create hotcopy at '%s'. "
                                   "The file '%s' is missing from the source "
                                   "repository. Please create this file, for "
                                   "instance by running 'svnadmin upgrade %s'"),
                                 dst_abspath, config_relpath, src_abspath);
              return svn_error_quick_wrap(err, msg);
            }
          else
            return svn_error_trace(err);
        }
    }

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* Find the youngest revision in the source and destination.
   * We only support hotcopies from sources with an equal or greater amount
   * of revisions than the destination.
   * This also catches the case where users accidentally swap the
   * source and destination arguments. */
  SVN_ERR(get_youngest(&src_youngest, src_fs->path, pool));
  if (incremental)
    {
      SVN_ERR(get_youngest(&dst_youngest, dst_fs->path, pool));
      if (src_youngest < dst_youngest)
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                 _("The hotcopy destination already contains more revisions "
                   "(%lu) than the hotcopy source contains (%lu); are source "
                   "and destination swapped?"),
                  dst_youngest, src_youngest);
    }
  else
    dst_youngest = 0;

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* Copy the min unpacked rev, and read its value. */
  if (src_ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
    {
      SVN_ERR(svn_fs_fs__read_min_unpacked_rev(&src_min_unpacked_rev,
                                               src_fs, pool));
      SVN_ERR(svn_fs_fs__read_min_unpacked_rev(&dst_min_unpacked_rev,
                                               dst_fs, pool));

      /* We only support packs coming from the hotcopy source.
       * The destination should not be packed independently from
       * the source. This also catches the case where users accidentally
       * swap the source and destination arguments. */
      if (src_min_unpacked_rev < dst_min_unpacked_rev)
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("The hotcopy destination already contains "
                                   "more packed revisions (%lu) than the "
                                   "hotcopy source contains (%lu)"),
                                   dst_min_unpacked_rev - 1,
                                   src_min_unpacked_rev - 1);

      SVN_ERR(svn_io_dir_file_copy(src_fs->path, dst_fs->path,
                                   PATH_MIN_UNPACKED_REV, pool));
    }
  else
    {
      src_min_unpacked_rev = 0;
      dst_min_unpacked_rev = 0;
    }

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /*
   * Copy the necessary rev files.
   */

  src_subdir = svn_dirent_join(src_fs->path, PATH_REVS_DIR, pool);
  dst_subdir = svn_dirent_join(dst_fs->path, PATH_REVS_DIR, pool);
  SVN_ERR(svn_io_make_dir_recursively(dst_subdir, pool));

  iterpool = svn_pool_create(pool);
  /* First, copy packed shards. */
  for (rev = 0; rev < src_min_unpacked_rev; rev += max_files_per_dir)
    {
      svn_error_t *err;

      svn_pool_clear(iterpool);

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      /* Copy the packed shard. */
      SVN_ERR(hotcopy_copy_packed_shard(&dst_min_unpacked_rev,
                                        src_fs, dst_fs,
                                        rev, max_files_per_dir,
                                        iterpool));

      /* If necessary, update 'current' to the most recent packed rev,
       * so readers can see new revisions which arrived in this pack. */
      SVN_ERR(hotcopy_update_current(&dst_youngest, dst_fs,
                                     rev + max_files_per_dir - 1,
                                     iterpool));

      /* Remove revision files which are now packed. */
      if (incremental)
        SVN_ERR(hotcopy_remove_rev_files(dst_fs, rev, rev + max_files_per_dir,
                                         max_files_per_dir, iterpool));

      /* Now that all revisions have moved into the pack, the original
       * rev dir can be removed. */
      err = svn_io_remove_dir2(svn_fs_fs__path_rev_shard(dst_fs, rev,
                                                         iterpool),
                               TRUE, cancel_func, cancel_baton, iterpool);
      if (err)
        {
          if (APR_STATUS_IS_ENOTEMPTY(err->apr_err))
            svn_error_clear(err);
          else
            return svn_error_trace(err);
        }
    }

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* Now, copy pairs of non-packed revisions and revprop files.
   * If necessary, update 'current' after copying all files from a shard. */
  SVN_ERR_ASSERT(rev == src_min_unpacked_rev);
  SVN_ERR_ASSERT(src_min_unpacked_rev == dst_min_unpacked_rev);
  revprop_src_subdir = svn_dirent_join(src_fs->path, PATH_REVPROPS_DIR, pool);
  revprop_dst_subdir = svn_dirent_join(dst_fs->path, PATH_REVPROPS_DIR, pool);
  SVN_ERR(svn_io_make_dir_recursively(revprop_dst_subdir, pool));
  for (; rev <= src_youngest; rev++)
    {
      svn_error_t *err;

      svn_pool_clear(iterpool);

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      /* Copy the rev file. */
      err = hotcopy_copy_shard_file(src_subdir, dst_subdir,
                                    rev, max_files_per_dir,
                                    iterpool);
      if (err)
        {
          if (APR_STATUS_IS_ENOENT(err->apr_err) &&
              src_ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
            {
              svn_error_clear(err);

              /* The source rev file does not exist. This can happen if the
               * source repository is being packed concurrently with this
               * hotcopy operation.
               *
               * If the new revision is now packed, and the youngest revision
               * we're interested in is not inside this pack, try to copy the
               * pack instead.
               *
               * If the youngest revision ended up being packed, don't try
               * to be smart and work around this. Just abort the hotcopy. */
              SVN_ERR(svn_fs_fs__update_min_unpacked_rev(src_fs, pool));
              if (svn_fs_fs__is_packed_rev(src_fs, rev))
                {
                  if (svn_fs_fs__is_packed_rev(src_fs, src_youngest))
                    return svn_error_createf(
                             SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("The assumed HEAD revision (%lu) of the "
                               "hotcopy source has been packed while the "
                               "hotcopy was in progress; please restart "
                               "the hotcopy operation"),
                             src_youngest);

                  SVN_ERR(hotcopy_copy_packed_shard(&dst_min_unpacked_rev,
                                                    src_fs, dst_fs,
                                                    rev, max_files_per_dir,
                                                    iterpool));
                  rev = dst_min_unpacked_rev;
                  continue;
                }
              else
                return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                                         _("Revision %lu disappeared from the "
                                           "hotcopy source while hotcopy was "
                                           "in progress"), rev);
            }
          else
            return svn_error_trace(err);
        }

      /* Copy the revprop file. */
      SVN_ERR(hotcopy_copy_shard_file(revprop_src_subdir,
                                      revprop_dst_subdir,
                                      rev, max_files_per_dir,
                                      iterpool));

      /* After completing a full shard, update 'current'. */
      if (max_files_per_dir && rev % max_files_per_dir == 0)
        SVN_ERR(hotcopy_update_current(&dst_youngest, dst_fs, rev, iterpool));
    }
  svn_pool_destroy(iterpool);

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* We assume that all revisions were copied now, i.e. we didn't exit the
   * above loop early. 'rev' was last incremented during exit of the loop. */
  SVN_ERR_ASSERT(rev == src_youngest + 1);

  /* All revisions were copied. Update 'current'. */
  SVN_ERR(hotcopy_update_current(&dst_youngest, dst_fs, src_youngest, pool));

  /* Replace the locks tree.
   * This is racy in case readers are currently trying to list locks in
   * the destination. However, we need to get rid of stale locks.
   * This is the simplest way of doing this, so we accept this small race. */
  dst_subdir = svn_dirent_join(dst_fs->path, PATH_LOCKS_DIR, pool);
  SVN_ERR(svn_io_remove_dir2(dst_subdir, TRUE, cancel_func, cancel_baton,
                             pool));
  src_subdir = svn_dirent_join(src_fs->path, PATH_LOCKS_DIR, pool);
  SVN_ERR(svn_io_check_path(src_subdir, &kind, pool));
  if (kind == svn_node_dir)
    SVN_ERR(svn_io_copy_dir_recursively(src_subdir, dst_fs->path,
                                        PATH_LOCKS_DIR, TRUE,
                                        cancel_func, cancel_baton, pool));

  /* Now copy the node-origins cache tree. */
  src_subdir = svn_dirent_join(src_fs->path, PATH_NODE_ORIGINS_DIR, pool);
  SVN_ERR(svn_io_check_path(src_subdir, &kind, pool));
  if (kind == svn_node_dir)
    SVN_ERR(hotcopy_io_copy_dir_recursively(src_subdir, dst_fs->path,
                                            PATH_NODE_ORIGINS_DIR, TRUE,
                                            cancel_func, cancel_baton, pool));

  /*
   * NB: Data copied below is only read by writers, not readers.
   *     Writers are still locked out at this point.
   */

  if (dst_ffd->format >= SVN_FS_FS__MIN_REP_SHARING_FORMAT)
    {
      /* Copy the rep cache and then remove entries for revisions
       * younger than the destination's youngest revision. */
      src_subdir = svn_dirent_join(src_fs->path, REP_CACHE_DB_NAME, pool);
      dst_subdir = svn_dirent_join(dst_fs->path, REP_CACHE_DB_NAME, pool);
      SVN_ERR(svn_io_check_path(src_subdir, &kind, pool));
      if (kind == svn_node_file)
        {
          SVN_ERR(svn_sqlite__hotcopy(src_subdir, dst_subdir, pool));
          SVN_ERR(svn_fs_fs__del_rep_reference(dst_fs, dst_youngest, pool));
        }
    }

  /* Copy the txn-current file. */
  if (dst_ffd->format >= SVN_FS_FS__MIN_TXN_CURRENT_FORMAT)
    SVN_ERR(svn_io_dir_file_copy(src_fs->path, dst_fs->path,
                                 PATH_TXN_CURRENT, pool));

  /* If a revprop generation file exists in the source filesystem,
   * reset it to zero (since this is on a different path, it will not
   * overlap with data already in cache).  Also, clean up stale files
   * used for the named atomics implementation. */
  SVN_ERR(svn_io_check_path(svn_fs_fs__path_revprop_generation(src_fs, pool),
                            &kind, pool));
  if (kind == svn_node_file)
    SVN_ERR(svn_fs_fs__write_revprop_generation_file(dst_fs, 0, pool));

  SVN_ERR(svn_fs_fs__cleanup_revprop_namespace(dst_fs));

  /* Hotcopied FS is complete. Stamp it with a format file. */
  SVN_ERR(write_format(svn_dirent_join(dst_fs->path, PATH_FORMAT, pool),
                       dst_ffd->format, max_files_per_dir, TRUE, pool));

  return SVN_NO_ERROR;
}


/* Set up shared data between SRC_FS and DST_FS. */
static void
hotcopy_setup_shared_fs_data(svn_fs_t *src_fs, svn_fs_t *dst_fs)
{
  fs_fs_data_t *src_ffd = src_fs->fsap_data;
  fs_fs_data_t *dst_ffd = dst_fs->fsap_data;

  /* The common pool and mutexes are shared between src and dst filesystems.
   * During hotcopy we only grab the mutexes for the destination, so there
   * is no risk of dead-lock. We don't write to the src filesystem. Shared
   * data for the src_fs has already been initialised in fs_hotcopy(). */
  dst_ffd->shared = src_ffd->shared;
}

/* Create an empty filesystem at DST_FS at DST_PATH with the same
 * configuration as SRC_FS (uuid, format, and other parameters).
 * After creation DST_FS has no revisions, not even revision zero. */
static svn_error_t *
hotcopy_create_empty_dest(svn_fs_t *src_fs,
                          svn_fs_t *dst_fs,
                          const char *dst_path,
                          apr_pool_t *pool)
{
  fs_fs_data_t *src_ffd = src_fs->fsap_data;
  fs_fs_data_t *dst_ffd = dst_fs->fsap_data;

  dst_fs->path = apr_pstrdup(pool, dst_path);

  dst_ffd->max_files_per_dir = src_ffd->max_files_per_dir;
  dst_ffd->config = src_ffd->config;
  dst_ffd->format = src_ffd->format;

  /* Create the revision data directories. */
  if (dst_ffd->max_files_per_dir)
    SVN_ERR(svn_io_make_dir_recursively(svn_fs_fs__path_rev_shard(dst_fs,
                                                                  0, pool),
                                        pool));
  else
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(dst_path,
                                                        PATH_REVS_DIR, pool),
                                        pool));

  /* Create the revprops directory. */
  if (src_ffd->max_files_per_dir)
    SVN_ERR(svn_io_make_dir_recursively(
                          svn_fs_fs__path_revprops_shard(dst_fs, 0, pool),
                          pool));
  else
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(dst_path,
                                                        PATH_REVPROPS_DIR,
                                                        pool),
                                        pool));

  /* Create the transaction directory. */
  SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(dst_path, PATH_TXNS_DIR,
                                                      pool),
                                      pool));

  /* Create the protorevs directory. */
  if (dst_ffd->format >= SVN_FS_FS__MIN_PROTOREVS_DIR_FORMAT)
    SVN_ERR(svn_io_make_dir_recursively(svn_dirent_join(dst_path,
                                                        PATH_TXN_PROTOS_DIR,
                                                        pool),
                                        pool));

  /* Create the 'current' file. */
  SVN_ERR(svn_io_file_create(svn_fs_fs__path_current(dst_fs, pool),
                             (dst_ffd->format >=
                                SVN_FS_FS__MIN_NO_GLOBAL_IDS_FORMAT
                                ? "0\n" : "0 1 1\n"),
                             pool));

  /* Create lock file and UUID. */
  SVN_ERR(svn_io_file_create(path_lock(dst_fs, pool), "", pool));
  SVN_ERR(svn_fs_fs__set_uuid(dst_fs, src_fs->uuid, pool));

  /* Create the min unpacked rev file. */
  if (dst_ffd->format >= SVN_FS_FS__MIN_PACKED_FORMAT)
    SVN_ERR(svn_io_file_create(svn_fs_fs__path_min_unpacked_rev(dst_fs, pool),
                                                                "0\n", pool));
  /* Create the txn-current file if the repository supports
     the transaction sequence file. */
  if (dst_ffd->format >= SVN_FS_FS__MIN_TXN_CURRENT_FORMAT)
    {
      SVN_ERR(svn_io_file_create(path_txn_current(dst_fs, pool),
                                 "0\n", pool));
      SVN_ERR(svn_io_file_create(path_txn_current_lock(dst_fs, pool),
                                 "", pool));
    }

  dst_ffd->youngest_rev_cache = 0;

  hotcopy_setup_shared_fs_data(src_fs, dst_fs);
  SVN_ERR(svn_fs_fs__initialize_caches(dst_fs, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__hotcopy(svn_fs_t *src_fs,
                   svn_fs_t *dst_fs,
                   const char *src_path,
                   const char *dst_path,
                   svn_boolean_t incremental,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *pool)
{
  struct hotcopy_body_baton hbb;

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_fs_fs__open(src_fs, src_path, pool));

  if (incremental)
    {
      const char *dst_format_abspath;
      svn_node_kind_t dst_format_kind;

      /* Check destination format to be sure we know how to incrementally
       * hotcopy to the destination FS. */
      dst_format_abspath = svn_dirent_join(dst_path, PATH_FORMAT, pool);
      SVN_ERR(svn_io_check_path(dst_format_abspath, &dst_format_kind, pool));
      if (dst_format_kind == svn_node_none)
        {
          /* Destination doesn't exist yet. Perform a normal hotcopy to a
           * empty destination using the same configuration as the source. */
          SVN_ERR(hotcopy_create_empty_dest(src_fs, dst_fs, dst_path, pool));
        }
      else
        {
          /* Check the existing repository. */
          SVN_ERR(svn_fs_fs__open(dst_fs, dst_path, pool));
          SVN_ERR(hotcopy_incremental_check_preconditions(src_fs, dst_fs,
                                                          pool));
          hotcopy_setup_shared_fs_data(src_fs, dst_fs);
          SVN_ERR(svn_fs_fs__initialize_caches(dst_fs, pool));
        }
    }
  else
    {
      /* Start out with an empty destination using the same configuration
       * as the source. */
      SVN_ERR(hotcopy_create_empty_dest(src_fs, dst_fs, dst_path, pool));
    }

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  hbb.src_fs = src_fs;
  hbb.dst_fs = dst_fs;
  hbb.incremental = incremental;
  hbb.cancel_func = cancel_func;
  hbb.cancel_baton = cancel_baton;
  SVN_ERR(svn_fs_fs__with_write_lock(dst_fs, hotcopy_body, &hbb, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__info_format(int *fs_format,
                       svn_version_t **supports_version,
                       svn_fs_t *fs,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  *fs_format = ffd->format;
  *supports_version = apr_palloc(result_pool, sizeof(svn_version_t));

  (*supports_version)->major = SVN_VER_MAJOR;
  (*supports_version)->minor = 1;
  (*supports_version)->patch = 0;
  (*supports_version)->tag = "";

  switch (ffd->format)
    {
    case 1:
      break;
    case 2:
      (*supports_version)->minor = 4;
      break;
    case 3:
      (*supports_version)->minor = 5;
      break;
    case 4:
      (*supports_version)->minor = 6;
      break;
    case 6:
      (*supports_version)->minor = 8;
      break;
#ifdef SVN_DEBUG
# if SVN_FS_FS__FORMAT_NUMBER != 6
#  error "Need to add a 'case' statement here"
# endif
#endif
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__info_config_files(apr_array_header_t **files,
                             svn_fs_t *fs,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  *files = apr_array_make(result_pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(*files, const char *) = svn_dirent_join(fs->path, PATH_CONFIG,
                                                         result_pool);
  return SVN_NO_ERROR;
}
