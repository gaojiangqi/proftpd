/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 * Copyright (C) 1999, 2000 MacGyver aka Habeeb J. Dihu <macgyver@tos.net>
 * Copyright (C) 2001, 2002, 2003 The ProFTPD Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */

/* ProFTPD virtual/modular file-system support
 * $Id: fsio.c,v 1.20 2003-05-22 19:26:06 castaglia Exp $
 */

#include "conf.h"

#ifdef HAVE_REGEX_H
# include <regex.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
# include <sys/statvfs.h>
#elif defined(HAVE_SYS_VFS_H)
# include <sys/vfs.h>
#elif defined(HAVE_SYS_MOUNT_H)
# include <sys/mount.h>
#endif

#ifdef AIX3
# include <sys/statfs.h>
#endif

typedef struct fsopendir fsopendir_t;

struct fsopendir {
  fsopendir_t *next,*prev;

  /* pool for this object's use */
  pool *pool;

  pr_fs_t *fsdir;
  DIR *dir;
};

static pr_fs_t *root_fs = NULL, *fs_cwd = NULL;
static array_header *fs_map = NULL;

#ifdef PR_FS_MATCH
static pr_fs_match_t *fs_match_list = NULL;
#endif /* PR_FS_MATCH */

static fsopendir_t *fsopendir_list;
static void *fs_cache_dir = NULL;
static pr_fs_t *fs_cache_fsdir = NULL;

/* Internal flag set whenever a new pr_fs_t has been added or removed, and
 * cleared once the fs_map has been scanned
 */
static unsigned char chk_fs_map = FALSE;

/* Virtual working directory */
static char vwd[MAXPATHLEN + 1] = "/";
static char cwd[MAXPATHLEN + 1] = "/";

/* The following static functions are simply wrappers for system functions
 */

static int sys_stat(pr_fs_t *fs, const char *path, struct stat *sbuf) {
  return stat(path, sbuf);
}

static int sys_fstat(pr_fh_t *fh, int fd, struct stat *sbuf) {
  return fstat(fd, sbuf);
}

static int sys_lstat(pr_fs_t *fs, const char *path, struct stat *sbuf) {
  return lstat(path, sbuf);
}

static int sys_rename(pr_fs_t *fs, const char *rnfm, const char *rnto) {
  return rename(rnfm, rnto);
}

static int sys_unlink(pr_fs_t *fs, const char *path) {
  return unlink(path);
}

static int sys_open(pr_fs_t *fs, const char *path, int flags) {

#ifdef CYGWIN
  /* On Cygwin systems, if we're about to read/write a binary file,
   * we need the open(2) equivalent of fopen(3)'s "b" option.  Cygwin
   * defines an O_BINARY flag for this purpose.
   */
  if (!(session.sf_flags & SF_ASCII))
    flags |= O_BINARY;
#endif

  return open(path, flags, PR_OPEN_MODE);
}

static int sys_creat(pr_fs_t *fs, const char *path, mode_t mode) {
  return creat(path, mode);
}

static int sys_close(pr_fh_t *fh, int fd) {
  return close(fd);
}

static int sys_read(pr_fh_t *fh, int fd, char *buf, size_t size) {
  return read(fd, buf, size);
}

static int sys_write(pr_fh_t *fh, int fd, const char *buf, size_t size) {
  return write(fd, buf, size);
}

static off_t sys_lseek(pr_fh_t *fh, int fd, off_t offset, int whence) {
  return lseek(fd, offset, whence);
}

static int sys_link(pr_fs_t *fs, const char *path1, const char *path2) {
  return link(path1, path2);
}

static int sys_symlink(pr_fs_t *fs, const char *path1, const char *path2) {
  return symlink(path1, path2);
}

static int sys_readlink(pr_fs_t *fs, const char *path, char *buf,
    size_t buflen) {
  return readlink(path, buf, buflen);
}

static int sys_ftruncate(pr_fh_t *fh, int fd, off_t len) {
  return ftruncate(fd, len);
}

static int sys_truncate(pr_fs_t *fs, const char *path, off_t len) {
  return truncate(path, len);
}

static int sys_chmod(pr_fs_t *fs, const char *path, mode_t mode) {
  return chmod(path, mode);
}

static int sys_chown(pr_fs_t *fs, const char *path, uid_t uid, gid_t gid) {
  return chown(path, uid, gid);
}

static int sys_chroot(pr_fs_t *fs, const char *path) {
  if (chroot(path) < 0)
    return -1;

  session.chroot_path = (char *) path;
  return 0;
}

static int sys_chdir(pr_fs_t *fs, const char *path) {
  if (chdir(path) < 0)
    return -1;

  pr_fs_setcwd(path);
  return 0;
}

static void *sys_opendir(pr_fs_t *fs, const char *path) {
  return opendir(path);
}

static int sys_closedir(pr_fs_t *fs, void *dir) {
  return closedir((DIR *) dir);
}

static struct dirent *sys_readdir(pr_fs_t *fs, void *dir) {
  return readdir((DIR *) dir);
}

static int sys_mkdir(pr_fs_t *fs, const char *path, mode_t mode) {
  return mkdir(path, mode);
}

static int sys_rmdir(pr_fs_t *fs, const char *path) {
  return rmdir(path);
}

static int fs_cmp(const void *a, const void *b) {
  pr_fs_t *fsa, *fsb;

  fsa = *((pr_fs_t **) a);
  fsb = *((pr_fs_t **) b);

  return strcmp(fsa->fs_path, fsb->fs_path);
}

/* Statcache stuff */
typedef struct {
  char sc_path[MAXPATHLEN+1];
  struct stat sc_stat;
  int sc_errno;

} fs_statcache_t;

static fs_statcache_t statcache;

#define fs_cache_lstat(f, p, s) cache_stat((f), (p), (s), FSIO_FILE_LSTAT)
#define fs_cache_stat(f, p, s) cache_stat((f), (p), (s), FSIO_FILE_STAT)

static int cache_stat(pr_fs_t *fs, const char *path, struct stat *sbuf,
    unsigned int op) {
  int res = -1;
  char pathbuf[MAXPATHLEN + 1] = {'\0'};
  int (*mystat)(pr_fs_t *, const char *, struct stat *) = NULL;

  /* Sanity checks */
  if (!fs) {
    errno = EINVAL;
    return -1;
  }

  if (!path) {
    errno = ENOENT;
    return -1;
  }

  /* Use only absolute path names.  Construct them, if given a relative
   * path, based on cwd.  This obviates the need for something like
   * realpath(3), which only introduces more stat system calls.
   */
  if (*path != '/') {
    sstrcat(pathbuf, cwd, MAXPATHLEN);
    sstrcat(pathbuf, "/", MAXPATHLEN);
    sstrcat(pathbuf, path, MAXPATHLEN);

  } else
    sstrncpy(pathbuf, path, MAXPATHLEN);

  /* Determine which filesystem function to use, stat() or lstat() */
  mystat = (op == FSIO_FILE_STAT) ? fs->stat : fs->lstat;

  /* Can the last cached stat be used? */
  if (!strcmp(pathbuf, statcache.sc_path)) {

    /* Update the given struct stat pointer with the cached info */
    memcpy(sbuf, &statcache.sc_stat, sizeof(struct stat));

    /* Use the cached errno as well */
    errno = statcache.sc_errno;

    return 0;
  }

  res = mystat(fs, pathbuf, sbuf);

  /* Update the cache */
  memset(statcache.sc_path, '\0', sizeof(statcache.sc_path));
  sstrncpy(statcache.sc_path, pathbuf, MAXPATHLEN);
  memcpy(&statcache.sc_stat, sbuf, sizeof(struct stat));
  statcache.sc_errno = errno;

  return res;
}

/* Lookup routines */

/* Necessary prototype for static function */
static pr_fs_t *fs_lookup_file_canon(const char *, char **, int);

/* fs_lookup_dir() is called when we want to perform some sort of directory
 * operation on a directory or file.  A "closest" match algorithm is used.  If
 * the lookup fails or is not "close enough" (i.e. the final target does not
 * exactly match an existing pr_fs_t) scan the list of fs_matches for
 * matchable targets and call any callback functions, then rescan the pr_fs_t
 * list.  The rescan is performed in case any modules registered pr_fs_ts
 * during the hit.
 */
static pr_fs_t *fs_lookup_dir(const char *path, int op) {
  char buf[MAXPATHLEN + 1] = {'\0'};
  char tmp_path[MAXPATHLEN + 1] = {'\0'};
  pr_fs_t *fs = NULL;
  int exact = FALSE;

#ifdef PR_FS_MATCH
  pr_fs_match_t *fsm = NULL;
#endif /* PR_FS_MATCH */

  sstrncpy(buf, path, sizeof(buf));

  if (buf[0] != '/')
    pr_fs_dircat(tmp_path, sizeof(tmp_path), cwd, buf);
  else
    sstrncpy(tmp_path, buf, sizeof(tmp_path));

  /* Make sure that if this is a directory operation, the path being
   * search ends in a trailing slash -- this is how files and directories
   * are differentiated in the fs_map.
   */
  if ((FSIO_DIR_COMMON & op) && tmp_path[strlen(tmp_path) - 1] != '/')
    sstrcat(tmp_path, "/", sizeof(tmp_path));

  fs = pr_get_fs(tmp_path, &exact);

#ifdef PR_FS_MATCH
/* NOTE: what if there is a perfect matching pr_fs_t for the given path,
 *  but an fs_match with pattern of "." is registered?  At present, that
 *  fs_match will never trigger...hmmm...OK.  fs_matches are only scanned
 *  if and only if there is *not* an exactly matching pr_fs_t.
 *
 *  NOTE: this is experimental code, not yet ready for module consumption.
 *  It was present in the older FS code, hence it's presence now.
 */

  /* Is the returned pr_fs_t "close enough"? */
  if (!fs || !exact) {

    /* Look for an fs_match */
    fsm = pr_get_fs_match(tmp_path, op);

    while (fsm) {

      /* Invoke the fs_match's callback function, if set
       *
       * NOTE: what pr_fs_t is being passed to the trigger??
       */
      if (fsm->trigger) {
        if (fsm->trigger(fs, tmp_path, op) <= 0)
          log_pri(LOG_DEBUG, "error: fs_match '%s' trigger failed",
            fsm->name);
      }

      /* Get the next matching fs_match */
      fsm = pr_get_next_fs_match(fsm, tmp_path, op);
    }
  }

  /* Now, check for a new pr_fs_t, if any were registered by fs_match
   * callbacks.  This time, it doesn't matter if it's an exact match --
   * any pr_fs_t will do.
   */
  if (chk_fs_map)
    fs = pr_get_fs(tmp_path, &exact);
#endif /* PR_FS_MATCH */

  return (fs ? fs : root_fs);
}

/* fs_lookup_file() performs the same function as fs_lookup_dir, however
 * because we are performing a file lookup, the target is the subdirectory
 * _containing_ the actual target.  A basic optimization is used here,
 * if the path contains no '/' characters, fs_cwd is returned.
 */
static pr_fs_t *fs_lookup_file(const char *path, char **deref, int op) {

  if (!strchr(path, '/')) {
#ifdef PR_FS_MATCH
    pr_fs_match_t *fsm = NULL;

    fsm = pr_get_fs_match(path, op);

    if (!fsm || fsm->trigger(fs_cwd, path, op) <= 0) {
#else
    if (1) {
#endif /* PR_FS_MATCH */
      struct stat sbuf;
      int (*mystat)(pr_fs_t *, const char *, struct stat *) = NULL;

      /* Determine which function to use, stat() or lstat(). */
      mystat = (op == FSIO_FILE_STAT) ? fs_cwd->stat : fs_cwd->lstat;

      if (mystat(fs_cwd, path, &sbuf) == -1 || !S_ISLNK(sbuf.st_mode))
        return fs_cwd;

    } else {

      /* The given path is a symbolic link, in which case we need to find
       * the actual path referenced, and return an pr_fs_t for _that_ path
       */
      char linkbuf[MAXPATHLEN + 1];
      int i;

      if (fs_cwd->readlink &&
          (i = fs_cwd->readlink(fs_cwd, path, &linkbuf[2], MAXPATHLEN-3)) != -1) {
        linkbuf[i] = '\0';
        if (!strchr(linkbuf, '/')) {
          if (i + 3 > MAXPATHLEN)
            i = MAXPATHLEN - 3;

          memmove(&linkbuf[2], linkbuf, i + 1);

          linkbuf[i+2] = '\0';
          linkbuf[0] = '.';
          linkbuf[1] = '/';
          return fs_lookup_file_canon(linkbuf, deref, op);
        }
      }

      /* What happens if fs_cwd->readlink is NULL, or readlink() returns -1?
       * I guess, for now, we punt, and return fs_cwd.
       */
      return fs_cwd;
    }
  }

  return fs_lookup_dir(path, op);
}

static pr_fs_t *fs_lookup_file_canon(const char *path, char **deref, int op) {
  static char workpath[MAXPATHLEN + 1];

  memset(workpath,'\0',sizeof(workpath));

  if (pr_fs_resolve_partial(path, workpath, MAXPATHLEN, FSIO_FILE_OPEN) == -1) {
    if (*path == '/' || *path == '~') {
      if (pr_fs_interpolate(path, workpath, MAXPATHLEN) != -1)
        sstrncpy(workpath, path, sizeof(workpath));

    } else
      pr_fs_dircat(workpath, sizeof(workpath), cwd, path);
  }

  if (deref)
    *deref = workpath;

  return fs_lookup_file(workpath, deref, op);
}

/* FS functions proper */

void pr_fs_clear_cache(void) {
  memset(&statcache, '\0', sizeof(statcache));
}

pr_fs_t *pr_register_fs(pool *p, const char *name, const char *path) {
  pr_fs_t *fs = NULL;

  /* Sanity check */
  if (!p || !name || !path) {
    errno = EINVAL;
    return NULL;
  }

  /* Instantiate an pr_fs_t */
  if ((fs = pr_create_fs(p, name)) != NULL) {

    /* Call pr_insert_fs() from here */
    if (!pr_insert_fs(fs, path)) {
      log_debug(DEBUG8, "FS: error inserting fs '%s' at path '%s'",
        name, path);

      destroy_pool(fs->fs_pool);
      return NULL;
    }

  } else
    log_debug(DEBUG8, "FS: error creating fs '%s'", name);

  return fs;
}

pr_fs_t *pr_create_fs(pool *p, const char *name) {
  pr_fs_t *fs = NULL;
  pool *rec_pool = NULL;

  /* Sanity check */
  if (!p || !name) {
    errno = EINVAL;
    return NULL;
  }

  /* Allocate a subpool, then allocate an pr_fs_t object from that subpool */
  rec_pool = make_sub_pool(p);
  fs = (pr_fs_t *) pcalloc(rec_pool, sizeof(pr_fs_t));

  if (!fs)
    return NULL;

  fs->fs_pool = rec_pool;

  /* Once layered pr_fs_ts are fully supported, this will be used/set,
   * probably in insert_fs(), as a linked-list of pr_fs_t's interested
   * in the same path.
   */
  fs->fs_next = fs->fs_prev = NULL;

  fs->fs_name = pstrdup(fs->fs_pool, name);

  /* This is NULL until set by pr_insert_fs() */
  fs->fs_path = NULL;

  /* Set the standard system FSIO functions, first.  The caller can provide
   * their own custom implementation function pointers using the returned
   * pr_fs_t pointer.
   */
  fs->stat = sys_stat;
  fs->fstat = sys_fstat;
  fs->lstat = sys_lstat;
  fs->rename = sys_rename;
  fs->unlink = sys_unlink;
  fs->open = sys_open;
  fs->creat = sys_creat;
  fs->close = sys_close;
  fs->read = sys_read;
  fs->write = sys_write;
  fs->lseek = sys_lseek;
  fs->link = sys_link;
  fs->readlink = sys_readlink;
  fs->symlink = sys_symlink;
  fs->ftruncate = sys_ftruncate;
  fs->truncate = sys_truncate;
  fs->chmod = sys_chmod;
  fs->chown = sys_chown;

  fs->chdir = sys_chdir;
  fs->chroot = sys_chroot;
  fs->opendir = sys_opendir;
  fs->closedir = sys_closedir;
  fs->readdir = sys_readdir;
  fs->mkdir = sys_mkdir;
  fs->rmdir = sys_rmdir;

  return fs;
}

int pr_insert_fs(pr_fs_t *fs, const char *path) {
  char cleaned_path[MAXPATHLEN] = {'\0'};

  if (!fs_map) {
    pool *map_pool = make_sub_pool(permanent_pool);
    fs_map = make_array(map_pool, 0, sizeof(pr_fs_t *));
  }

  /* Clean the path, but only if it starts with a '/'.  Non-local-filesystem
   * paths may not want/need to be cleaned.
   */
  if (*path == '/')
    pr_fs_clean_path(path, cleaned_path, sizeof(cleaned_path));
  else
    sstrncpy(cleaned_path, path, sizeof(cleaned_path));

  if (!fs->fs_path)
    fs->fs_path = pstrdup(fs->fs_pool, cleaned_path);

  /* For now, disallow any attempts at layering, meaning no duplicate
   * paths.  Once layering of FS module/pr_fs_ts is allowed, distinguish
   * between pr_fs_ts of identical paths by name, and extend the check to
   * bar registration of pr_fs_ts with existing name/path combinations.
   */
  if (fs_map->nelts > 0) {
    pr_fs_t *fsi = NULL, **fs_objs = (pr_fs_t **) fs_map->elts;
    register int i;

    for (i = 0; i < fs_map->nelts; i++) {
      fsi = fs_objs[i];
      if (!strcmp(fsi->fs_path, cleaned_path)) {
        log_pri(LOG_DEBUG, "error: duplicate fs paths not allowed: '%s'",
          cleaned_path);
        return FALSE;
      }
    }
  }

  /* Push the new pr_fs_t into the container, then resort the contents. */
  *((pr_fs_t **) push_array(fs_map)) = fs;

  /* Sort the pr_fs_ts in the map according to their paths (if there are
   * more than one element in the array_header.
   */
  if (fs_map->nelts > 1)
    qsort(fs_map->elts, fs_map->nelts, sizeof(pr_fs_t *), fs_cmp);

  /* Set the flag so that the fs wrapper functions know that a new pr_fs_t
   * has been registered.
   */
  chk_fs_map = TRUE;

  return TRUE;
}

int pr_unregister_fs(const char *path) {
  pr_fs_t *fs = NULL, **fs_objs = NULL;
  register unsigned int i = 0;

  /* Sanity check */
  if (!path) {
    errno = EINVAL;
    return FALSE;
  }

  /* This should never be called before pr_register_fs(), but, just in case...*/
  if (!fs_map) {
    errno = EACCES;
    return FALSE;
  }

  fs_objs = (pr_fs_t **) fs_map->elts;

  for (i = 0; i < fs_map->nelts; i++) {
    fs = fs_objs[i];

    if (!strcmp(fs->fs_path, path)) {
      register unsigned int j = 0;

      /* Exact match -- remove this pr_fs_t.  Allocate a new map. Iterate
       * through the old map, pushing all other pr_fs_ts into the new map.
       * Destroy this pr_fs_t's pool.  Destroy the old map.  Move the new map
       * into place.
       */

      pr_fs_t *tmp_fs, **old_objs = NULL;

      pool *map_pool = make_sub_pool(permanent_pool);
      array_header *new_map = make_array(map_pool, 0, sizeof(pr_fs_t *));
      old_objs = (pr_fs_t **) fs_map->elts;

      for (j = 0; j < fs_map->nelts; j++) {
        tmp_fs = old_objs[j];

        if (strcmp(tmp_fs->fs_path, path))
          *((pr_fs_t **) push_array(new_map)) = old_objs[j];
      }

      destroy_pool(fs->fs_pool);
      destroy_pool(fs_map->pool);

      fs_map = new_map;

      /* Don't forget to set the flag so that wrapper functions scan the
       * new map.
       */
      chk_fs_map = TRUE;

      return TRUE;
    }
  }

  return FALSE;
}

/* This function returns the best pr_fs_t to handle the given path.  It will
 * return NULL if there are no registered pr_fs_ts to handle the given path,
 * in which case the default root_fs should be used.  This is so that
 * functions can look to see if an pr_fs_t, other than the default, for a
 * given path has been registered, if necessary.  If the return value is
 * non-NULL, that will be a registered pr_fs_t to handle the given path.  In
 * this case, if the exact argument is not NULL, it will either be TRUE,
 * signifying that the returned pr_fs_t is an exact match for the given
 * path, or FALSE, meaning the returned pr_fs_t is a "best match" -- most
 * likely the pr_fs_t that handles the directory in which the given path
 * occurs.
 */
pr_fs_t *pr_get_fs(const char *path, int *exact) {
  pr_fs_t *fs = NULL, **fs_objs = NULL, *best_match_fs = NULL;
  register unsigned int i = 0;

  /* Sanity check */
  if (!path) {
    errno = EINVAL;
    return NULL;
  }

  /* Basic optimization -- if there're no elements in the fs_map,
   * return the root_fs.
   */
  if (!fs_map || fs_map->nelts == 0)
    return root_fs;

  fs_objs = (pr_fs_t **) fs_map->elts;
  best_match_fs = fs_objs[0];

  /* In order to handle deferred-resolution paths (eg "~" paths), the given
   * path will need to be passed through dir_realpath(), if necessary.
   */

  /* The chk_fs_map flag, if TRUE, should be cleared on return of this
   * function -- all that flag says is, if TRUE, that this function _might_
   * return something different than it did on a previous call
   */

  for (i = 0; i < fs_map->nelts; i++) {
    int res = 0;

    fs = fs_objs[i];

    /* If the current pr_fs_t's path ends in a slash (meaning it is a
     * directory, and it matches the first part of the given path,
     * assume it to be the best pr_fs_t found so far.
     */
    if ((fs->fs_path)[strlen(fs->fs_path) - 1] == '/' &&
        !strncmp(path, fs->fs_path, strlen(fs->fs_path)))
      best_match_fs = fs;

    res = strcmp(fs->fs_path, path);

    if (res == 0) {

      /* Exact match */
      if (exact)
        *exact = TRUE;

      chk_fs_map = FALSE;
      return fs;

    } else if (res > 0) {

      if (exact)
        *exact = FALSE;

      chk_fs_map = FALSE;

      /* Gone too far - return the best-match pr_fs_t */
      return best_match_fs;
    }
  }

  chk_fs_map = FALSE;

  /* Return best-match by default */
  return best_match_fs;
}

#if defined(HAVE_REGEX_H) && defined(HAVE_REGCOMP)
#ifdef PR_FS_MATCH
void pr_associate_fs(pr_fs_match_t *fsm, pr_fs_t *fs) {
  *((pr_fs_t **) push_array(fsm->fsm_fs_objs)) = fs;
}

pr_fs_match_t *pr_create_fs_match(pool *p, const char *name,
    const char *pattern, int opmask) {
  pr_fs_match_t *fsm = NULL;
  pool *match_pool = NULL;
  regex_t *regexp = NULL;
  int res = 0;
  char regerr[80] = {'\0'};

  if (!p || !name || !pattern) {
    errno = EINVAL;
    return NULL;
  }

  match_pool = make_sub_pool(p);
  fsm = (pr_fs_match_t *) pcalloc(match_pool, sizeof(pr_fs_match_t));

  if (!fsm)
    return NULL;

  fsm->fsm_next = NULL;
  fsm->fsm_prev = NULL;

  fsm->fsm_pool = match_pool;
  fsm->fsm_name = pstrdup(fsm->fsm_pool, name);
  fsm->fsm_opmask = opmask;
  fsm->fsm_pattern = pstrdup(fsm->fsm_pool, pattern);

  regexp = pr_regexp_alloc();

  if ((res = regcomp(regexp, pattern, REG_EXTENDED|REG_NOSUB)) != 0) {

    regerror(res, regexp, regerr, sizeof(regerr));
    pr_regexp_free(regexp);

    log_pri(LOG_ERR, "unable to compile regex '%s': %s", pattern, regerr);

    /* Destroy the just allocated pr_fs_match_t */
    destroy_pool(fsm->fsm_pool);

    return NULL;

  } else
    fsm->fsm_regex = regexp;

  /* All pr_fs_match_ts start out as null patterns, i.e. no defined callback.
   */
  fsm->trigger = NULL;

  /* Allocate an array_header, used to record the pointers of any pr_fs_ts
   * this pr_fs_match_t may register.  This array_header should be accessed
   * via associate_fs().
   */
  fsm->fsm_fs_objs = make_array(fsm->fsm_pool, 0, sizeof(pr_fs_t *));

  return fsm;
}

int pr_insert_fs_match(pr_fs_match_t *fsm) {
  pr_fs_match_t *fsmi = NULL;

  if (fs_match_list) {

    /* Find the end of the fs_match list */
    fsmi = fs_match_list;

    /* Prevent pr_fs_match_ts with duplicate names */
    if (!strcmp(fsmi->fsm_name, fsm->fsm_name)) {
      log_pri(LOG_DEBUG, "error: duplicate fs_match names not allowed: '%s'",
        fsm->fsm_name);
      return FALSE;
    }

    while (fsmi->fsm_next) {
      fsmi = fsmi->fsm_next;

      if (!strcmp(fsmi->fsm_name, fsm->fsm_name)) {
        log_pri(LOG_DEBUG, "error: duplicate fs_match names not allowed: '%s'",
          fsm->fsm_name);
        return FALSE;
      }
    }

    fsm->fsm_next = NULL;
    fsm->fsm_prev = fsmi;
    fsmi->fsm_next = fsm;

  } else

    /* This fs_match _becomes_ the start of the fs_match list */
    fs_match_list = fsm;

  return TRUE;
}

pr_fs_match_t *pr_register_fs_match(pool *p, const char *name,
    const char *pattern, int opmask) {
  pr_fs_match_t *fsm = NULL;

  /* Sanity check */
  if (!p || !name || !pattern) {
    errno = EINVAL;
    return NULL;
  }

  /* Instantiate an fs_match */
  if ((fsm = pr_create_fs_match(p, name, pattern, opmask)) != NULL) {

    /* Insert the fs_match into the list */
    if (!pr_insert_fs_match(fsm)) {
      pr_regexp_free(fsm->fsm_regex);
      destroy_pool(fsm->fsm_pool);

      return NULL;
    }
  }

  return fsm;
}

int pr_unregister_fs_match(const char *name) {
  pr_fs_match_t *fsm = NULL;
  pr_fs_t **assoc_fs_objs = NULL, *assoc_fs = NULL;
  int removed = FALSE;

  /* fs_matches are required to have duplicate names, so using the name as
   * the identifier will work.
   */

  /* Sanity check*/
  if (!name) {
    errno = EINVAL;
    return FALSE;
  }

  if (fs_match_list) {
    for (fsm = fs_match_list; fsm; fsm = fsm->fsm_next) {

      /* Search by name */
      if ((name && fsm->fsm_name && !strcmp(fsm->fsm_name, name))) {

        /* Remove this fs_match from the list */
        if (fsm->fsm_prev)
          fsm->fsm_prev->fsm_next = fsm->fsm_next;

        if (fsm->fsm_next)
          fsm->fsm_next->fsm_prev = fsm->fsm_prev;

        /* Check for any pr_fs_ts this pattern may have registered, and
         * remove them as well.
         */
        assoc_fs_objs = (pr_fs_t **) fsm->fsm_fs_objs->elts;

        for (assoc_fs = *assoc_fs_objs; assoc_fs; assoc_fs++)
          pr_unregister_fs(assoc_fs->fs_path);

        pr_regexp_free(fsm->fsm_regex);
        destroy_pool(fsm->fsm_pool);

        /* If this fs_match's prev and next pointers are NULL, it is the
         * last fs_match in the list.  If this is the case, make sure
         * that fs_match_list is set to NULL, signalling that there are
         * no more registered fs_matches.
         */
        if (fsm->fsm_prev == NULL && fsm->fsm_next == NULL) {
          fs_match_list = NULL;
          fsm = NULL;
        }

        removed = TRUE;
      }
    }
  }

  return (removed ? TRUE : FALSE);
}

pr_fs_match_t *pr_get_next_fs_match(pr_fs_match_t *fsm, const char *path,
    int op) {
  pr_fs_match_t *fsmi = NULL;

  /* Sanity check */
  if (!fsm) {
    errno = EINVAL;
    return NULL;
  }

  for (fsmi = fsm->fsm_next; fsmi; fsmi = fsmi->fsm_next) {
    if ((fsmi->fsm_opmask & op) &&
        regexec(fsmi->fsm_regex, path, 0, NULL, 0) == 0)
      return fsmi;
  }

  return NULL;
}

pr_fs_match_t *pr_get_fs_match(const char *path, int op) {
  pr_fs_match_t *fsm = NULL;

  if (!fs_match_list)
    return NULL;

  /* Check the first element in the fs_match_list... */
  fsm = fs_match_list;

  if ((fsm->fsm_opmask & op) &&
      regexec(fsm->fsm_regex, path, 0, NULL, 0) == 0)
    return fsm;

  /* ...otherwise, hand the search off to pr_get_next_fs_match() */
  return pr_get_next_fs_match(fsm, path, op);
}
#endif /* PR_FS_MATCH */
#endif /* HAVE_REGEX_H && HAVE_REGCOMP */

void pr_fs_setcwd(const char *dir) {
  pr_fs_resolve_path(dir, cwd, MAXPATHLEN, FSIO_DIR_CHDIR);
  sstrncpy(cwd, dir, sizeof(cwd));
  fs_cwd = fs_lookup_dir(cwd, FSIO_DIR_CHDIR);
  cwd[sizeof(cwd) - 1] = '\0';
}

const char *pr_fs_getcwd(void) {
  return cwd;
}

const char *pr_fs_getvwd(void) {
  return vwd;
}

void pr_fs_dircat(char *buf, int len, const char *dir1, const char *dir2) {
  /* make temporary copies so that memory areas can overlap */
  char *_dir1 = NULL, *_dir2 = NULL;
  size_t i = 0;

  /* This is an experimental test to see if we've got reasonable
   * directories to concatenate.  If we don't, then we default to
   * the root directory.
   */
  if ((strlen(dir1) + strlen(dir2) + 1) > MAXPATHLEN) {
    sstrncpy(buf, "/", len);
    return;
  }

  _dir1 = strdup(dir1);
  _dir2 = strdup(dir2);

  i = strlen(_dir1) - 1;

  if (*_dir2 == '/') {
    sstrncpy(buf, _dir2, len);
    free(_dir1);
    free(_dir2);
    return;
  }

  sstrncpy(buf, _dir1, len);

  if (len && *(_dir1 + i) != '/')
    sstrcat(buf, "/", MAXPATHLEN);

  sstrcat(buf, _dir2, MAXPATHLEN);

  if (!*buf) {
   *buf++ = '/';
   *buf = '\0';
  }

  free(_dir1);
  free(_dir2);
}

/* This function performs any tilde expansion needed and then returns the
 * resolved path, if any.
 *
 * Returns: -1 (errno = ENOENT): user does not exist
 *           0 : no interpolation done (path exists)
 *           1 : interpolation done
 */
int pr_fs_interpolate(const char *path, char *buf, size_t buflen) {
  pool *p = NULL;
  struct passwd *pw = NULL;
  struct stat sbuf;
  char *fname = NULL;
  char user[MAXPATHLEN + 1] = {'\0'};
  int len;

  if (!path) {
    errno = EINVAL;
    return -1;
  }

  if (path[0] == '~') {
    fname = strchr(path, '/');

    /* Copy over the username.
     */
    if (fname) {
      len = fname - path;
      sstrncpy(user, path + 1, len > sizeof(user) ? sizeof(user) : len);

      /* Advance past the '/'. */
      fname++;

    } else if (pr_fsio_stat(path, &sbuf) == -1) {

      /* Otherwise, this might be something like "~foo" which could be a file
       * or it could be a user.  Let's find out.
       *
       * Must be a user, if anything...otherwise it's probably a typo.
       */
      len = strlen(path);
      sstrncpy(user, path + 1, len > sizeof(user) ? sizeof(user) : len);

    } else {

      /* Otherwise, this _is_ the file in question, perform no interpolation.
       */
      fname = (char *) path;
      return 0;
    }

    /* If the user hasn't been explicitly specified, set it here.  This
     * handles cases such as files beginning with "~", "~/foo" or simply "~".
     */
    if (!*user)
      sstrncpy(user, session.user, sizeof(user));

    p = make_sub_pool(permanent_pool);
    pw = auth_getpwnam(p, user);
    destroy_pool(p);

    if (!pw) {
      errno = ENOENT;
      return -1;
    }

    sstrncpy(buf, pw->pw_dir, buflen);
    len = strlen(buf);

    if (fname && len < buflen && buf[len - 1] != '/')
      buf[len++] = '/';

    if (fname)
      sstrncpy(&buf[len], fname, buflen - len);

  } else
    sstrncpy(buf, path, buflen);

  return 1;
}

int pr_fs_resolve_partial(const char *path, char *buf, size_t buflen, int op) {
  char curpath[MAXPATHLEN + 1]  = {'\0'},
       workpath[MAXPATHLEN + 1] = {'\0'},
       namebuf[MAXPATHLEN + 1]  = {'\0'},
       linkpath[MAXPATHLEN + 1] = {'\0'},
       *where = NULL, *ptr = NULL, *last = NULL;

  pr_fs_t *fs = NULL;
  int len = 0, fini = 1, link_cnt = 0;
  ino_t last_inode = 0;
  struct stat sbuf;

  if (!path) {
    errno = EINVAL;
    return -1;
  }

  if (*path != '/') {
    if (*path == '~') {
      switch (pr_fs_interpolate(path, curpath, sizeof(curpath))) {
      case -1:
        return -1;

      case 0:
        sstrncpy(curpath, path, sizeof(curpath));
        sstrncpy(workpath, cwd, sizeof(workpath));
        break;
      }

    } else {
      sstrncpy(curpath, path, sizeof(curpath));
      sstrncpy(workpath, cwd, sizeof(workpath));
    }

  } else
    sstrncpy(curpath, path, sizeof(curpath));

  while (fini--) {
    where = curpath;
    while (*where != '\0') {

      /* Handle "." */
      if (!strcmp(where, ".")) {
        where++;
        continue;
      }

      /* Handle ".." */
      if (!strcmp(where, "..")) {
        where += 2;
        ptr = last = workpath;

        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }

        *last = '\0';
        continue;
      }

      /* Handle "./" */
      if (!strncmp(where, "./", 2)) {
        where += 2;
        continue;
      }

      /* Handle "../" */
      if (!strncmp(where, "../", 3)) {
        where += 3;
        ptr = last = workpath;

        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }

        *last = '\0';
        continue;
      }

      ptr = strchr(where, '/');

      if (!ptr)
        ptr = where + strlen(where) - 1;
      else
        *ptr = '\0';

      sstrncpy(namebuf, workpath, sizeof(namebuf));

      if (*namebuf) {
        for (last = namebuf; *last; last++);
        if (*--last != '/')
          sstrcat(namebuf, "/", MAXPATHLEN);

      } else
        sstrcat(namebuf, "/", MAXPATHLEN);

      sstrcat(namebuf, where, MAXPATHLEN);

      where = ++ptr;

      fs = fs_lookup_dir(namebuf, op);

      if (fs_cache_lstat(fs, namebuf, &sbuf) == -1)
        return -1;

      if (S_ISLNK(sbuf.st_mode)) {
        /* Detect an obvious recursive symlink */
        if (sbuf.st_ino && (ino_t) sbuf.st_ino == last_inode) {
          errno = ELOOP;
          return -1;
        }

        last_inode = (ino_t) sbuf.st_ino;
        if (++link_cnt > 32) {
          errno = ELOOP;
          return -1;
        }
	
        if (fs->readlink &&
            (len = fs->readlink(fs, namebuf, linkpath, MAXPATHLEN)) <= 0) {
          errno = ENOENT;
          return -1;
        }

        *(linkpath + len) = '\0';
        if (*linkpath == '/')
          *workpath = '\0';

        if (*linkpath == '~') {
          char tmpbuf[MAXPATHLEN + 1] = {'\0'};

          *workpath = '\0';
          sstrncpy(tmpbuf, linkpath, sizeof(tmpbuf));

          if (pr_fs_interpolate(tmpbuf, linkpath, sizeof(linkpath)) == -1)
	    return -1;
        }

        if (*where) {
          sstrcat(linkpath, "/", MAXPATHLEN);
          sstrcat(linkpath, where, MAXPATHLEN);
        }

        sstrncpy(curpath, linkpath, sizeof(curpath));
        fini++;
        break; /* continue main loop */
      }

      if (S_ISDIR(sbuf.st_mode)) {
        sstrncpy(workpath, namebuf, sizeof(workpath));
        continue;
      }

      if (*where) {
        errno = ENOENT;
        return -1;               /* path/notadir/morepath */

      } else {
        sstrncpy(workpath, namebuf, sizeof(workpath));
      }
    }
  }

  if (!workpath[0])
    sstrncpy(workpath, "/", sizeof(workpath));

  sstrncpy(buf, workpath, buflen);

  return 0;
}

int pr_fs_resolve_path(const char *path, char *buf, size_t buflen, int op) {
  char curpath[MAXPATHLEN + 1]  = {'\0'},
       workpath[MAXPATHLEN + 1] = {'\0'},
       namebuf[MAXPATHLEN + 1]  = {'\0'},
       linkpath[MAXPATHLEN + 1] = {'\0'},
       *where = NULL, *ptr = NULL, *last = NULL;

  pr_fs_t *fs = NULL;

  int len = 0, fini = 1, link_cnt = 0;
  ino_t last_inode = 0;
  struct stat sbuf;

  if (!path) {
    errno = EINVAL;
    return -1;
  }

  if (pr_fs_interpolate(path, curpath, MAXPATHLEN) != -1)
    sstrncpy(curpath, path, sizeof(curpath));

  if (curpath[0] != '/')
    sstrncpy(workpath, cwd, sizeof(workpath));
  else
    workpath[0] = '\0';

  while (fini--) {
    where = curpath;

    while (*where != '\0') {
      if (!strcmp(where, ".")) {
        where++;
        continue;
      }

      /* handle "./" */
      if (!strncmp(where, "./", 2)) {
        where += 2;
        continue;
      }

      /* handle "../" */
      if (!strncmp(where, "../", 3)) {
        where += 3;
        ptr = last = workpath;
        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }

        *last = '\0';
        continue;
      }

      ptr = strchr(where, '/');

      if (!ptr)
        ptr = where + strlen(where) - 1;
      else
        *ptr = '\0';

      sstrncpy(namebuf, workpath, sizeof(namebuf));

      if (*namebuf) {
        for (last = namebuf; *last; last++);

        if (*--last != '/')
          sstrcat(namebuf, "/", MAXPATHLEN);

      } else
        sstrcat(namebuf, "/", MAXPATHLEN);

      sstrcat(namebuf, where, MAXPATHLEN);

      where = ++ptr;

      fs = fs_lookup_dir(namebuf, op);

      if (fs_cache_lstat(fs, namebuf, &sbuf) == -1) {
        errno = ENOENT;
        return -1;
      }

      if (S_ISLNK(sbuf.st_mode)) {
        /* Detect an obvious recursive symlink */
        if (sbuf.st_ino && (ino_t) sbuf.st_ino == last_inode) {
          errno = ENOENT;
          return -1;
        }

        last_inode = (ino_t) sbuf.st_ino;

        if (++link_cnt > 32) {
          errno = ELOOP;
          return -1;
        }

        if (fs->readlink &&
            (len = fs->readlink(fs, namebuf, linkpath, MAXPATHLEN)) <= 0) {
          errno = ENOENT;
          return -1;
        }

        *(linkpath+len) = '\0';

        if (*linkpath == '/')
          *workpath = '\0';

        if (*linkpath == '~') {
          char tmpbuf[MAXPATHLEN + 1] = {'\0'};
          *workpath = '\0';

          sstrncpy(tmpbuf, linkpath, sizeof(tmpbuf));

          if (pr_fs_interpolate(tmpbuf, linkpath, sizeof(linkpath)) == -1)
	    return -1;
        }

        if (*where) {
          sstrcat(linkpath, "/", MAXPATHLEN);
          sstrcat(linkpath, where, MAXPATHLEN);
        }

        sstrncpy(curpath, linkpath, sizeof(curpath));
        fini++;
        break; /* continue main loop */
      }

      if (S_ISDIR(sbuf.st_mode)) {
        sstrncpy(workpath, namebuf, sizeof(workpath));
        continue;
      }

      if (*where) {
        errno = ENOENT;
        return -1;               /* path/notadir/morepath */

      } else
        sstrncpy(workpath, namebuf, sizeof(workpath));
    }
  }

  if (!workpath[0])
    sstrncpy(workpath, "/", sizeof(workpath));

  sstrncpy(buf, workpath, buflen);

  return 0;
}

void pr_fs_clean_path(const char *path, char *buf, size_t buflen) {
  char workpath[MAXPATHLEN + 1] = {'\0'};
  char curpath[MAXPATHLEN + 1]  = {'\0'};
  char namebuf[MAXPATHLEN + 1]  = {'\0'};
  char *where = NULL, *ptr = NULL, *last = NULL;
  int fini = 1;

  if (!path)
    return;

  sstrncpy(curpath, path, sizeof(curpath));

  /* main loop */
  while (fini--) {
    where = curpath;
    while (*where != '\0') {
      if (!strcmp(where, ".")) {
        where++;
        continue;
      }

      /* handle "./" */
      if (!strncmp(where, "./", 2)) {
        where += 2;
        continue;
      }

      /* handle ".." */
      if (!strcmp(where, "..")) {
        where += 2;
        ptr = last = workpath;

        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }

        *last = '\0';
        continue;
      }

      /* handle "../" */
      if (!strncmp(where, "../", 3)) {
        where += 3;
        ptr = last = workpath;

        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }
        *last = '\0';
        continue;
      }
      ptr = strchr(where, '/');

      if (!ptr)
        ptr = where + strlen(where) - 1;
      else
        *ptr = '\0';

      sstrncpy(namebuf, workpath, sizeof(namebuf));

      if (*namebuf) {
        for (last = namebuf; *last; last++);
        if (*--last != '/')
          sstrcat(namebuf, "/", MAXPATHLEN);

      } else
        sstrcat(namebuf, "/", MAXPATHLEN);

      sstrcat(namebuf, where, MAXPATHLEN);
      namebuf[MAXPATHLEN-1] = '\0';

      where = ++ptr;

      sstrncpy(workpath, namebuf, sizeof(workpath));
    }
  }

  if (!workpath[0])
    sstrncpy(workpath, "/", sizeof(workpath));

  sstrncpy(buf, workpath, buflen);
}

/* This function checks the given path's prefix against the paths that
 * have been registered.  If no matching path prefix has been registered,
 * the path is considered invalid.
 */
int pr_fs_valid_path(const char *path) {

  if (fs_map && fs_map->nelts > 0) {
    pr_fs_t *fsi = NULL, **fs_objs = (pr_fs_t **) fs_map->elts;
    register int i;

    for (i = 0; i < fs_map->nelts; i++) {
      fsi = fs_objs[i];

      if (strncmp(fsi->fs_path, path, strlen(fsi->fs_path)) == 0)
        return 0;
    }
  }

  /* Also check the path against the default '/' path. */
  if (*path == '/')
    return 0;

  errno = EINVAL;
  return -1;
}

void pr_fs_virtual_path(const char *path, char *buf, size_t buflen) {
  char curpath[MAXPATHLEN + 1]  = {'\0'},
       workpath[MAXPATHLEN + 1] = {'\0'},
       namebuf[MAXPATHLEN + 1]  = {'\0'},
       *where = NULL, *ptr = NULL, *last = NULL;

  int fini = 1;

  if (!path)
    return;

  if (pr_fs_interpolate(path, curpath, MAXPATHLEN) != -1)
    sstrncpy(curpath, path, sizeof(curpath));

  if (curpath[0] != '/')
    sstrncpy(workpath, vwd, sizeof(workpath));
  else
    workpath[0] = '\0';

  /* curpath is path resolving */
  /* linkpath is path a symlink pointed to */
  /* workpath is the path we've resolved */

  /* main loop */
  while (fini--) {
    where = curpath;
    while (*where != '\0') {
      if (!strcmp(where, ".")) {
        where++;
        continue;
      }

      /* handle "./" */
      if (!strncmp(where, "./", 2)) {
        where += 2;
        continue;
      }

      /* handle ".." */
      if (!strcmp(where, "..")) {
        where += 2;
        ptr = last = workpath;
        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }

        *last = '\0';
        continue;
      }

      /* handle "../" */
      if (!strncmp(where, "../", 3)) {
        where += 3;
        ptr = last = workpath;
        while (*ptr) {
          if (*ptr == '/')
            last = ptr;
          ptr++;
        }
        *last = '\0';
        continue;
      }
      ptr = strchr(where, '/');

      if (!ptr)
        ptr = where + strlen(where) - 1;
      else
        *ptr = '\0';

      sstrncpy(namebuf, workpath, sizeof(namebuf));

      if (*namebuf) {
        for (last = namebuf; *last; last++);
        if (*--last != '/')
          sstrcat(namebuf, "/", MAXPATHLEN);

      } else
        sstrcat(namebuf, "/", MAXPATHLEN);

      sstrcat(namebuf, where, MAXPATHLEN);

      where = ++ptr;

      sstrncpy(workpath, namebuf, sizeof(workpath));
    }
  }

  if (!workpath[0])
    sstrncpy(workpath, "/", sizeof(workpath));

  sstrncpy(buf, workpath, buflen);
}

int pr_fsio_chdir_canon(const char *path, int hidesymlink) {
  char resbuf[MAXPATHLEN + 1] = {'\0'};
  pr_fs_t *fs = NULL;
  int res = 0;

  if (pr_fs_resolve_partial(path, resbuf, MAXPATHLEN, FSIO_DIR_CHDIR) == -1)
    return -1;

  fs = fs_lookup_dir(resbuf, FSIO_DIR_CHDIR);

  if (fs->chdir) {
    log_debug(DEBUG9, "FS: using %s chdir()",
      fs->chdir == sys_chdir ? "system" : fs->fs_name);
    res = fs->chdir(fs, resbuf);

  } else {
    errno = EPERM;
    return -1;
  }

  if (res != -1) {
    /* chdir succeeded, so we set fs_cwd for future references. */
     fs_cwd = fs;

     if (hidesymlink)
       pr_fs_virtual_path(path, vwd, MAXPATHLEN);
     else
       sstrncpy(vwd, resbuf, sizeof(vwd));
  }

  return res;
}

int pr_fsio_chdir(const char *path, int hidesymlink) {
  char resbuf[MAXPATHLEN + 1] = {'\0'};
  pr_fs_t *fs = NULL;
  int res;

  pr_fs_clean_path(path, resbuf, MAXPATHLEN);

  fs = fs_lookup_dir(path, FSIO_DIR_CHDIR);

  if (fs->chdir) {
    log_debug(DEBUG9, "FS: using %s chdir()",
      fs->chdir == sys_chdir ? "system" : fs->fs_name);
    res = fs->chdir(fs, resbuf);

  } else {
    errno = EPERM;
    return -1;
  }

  if (res != -1) {
    /* chdir succeeded, so we set fs_cwd for future references. */
     fs_cwd = fs;

     if (hidesymlink)
       pr_fs_virtual_path(path, vwd, MAXPATHLEN);
     else
       sstrncpy(vwd, resbuf, sizeof(vwd));
  }

  return res;
}

/* fs_opendir, fs_closedir and fs_readdir all use a nifty
 * optimization, caching the last-recently-used pr_fs_t, and
 * avoid future pr_fs_t lookups when iterating via readdir.
 */
void *pr_fsio_opendir(const char *path) {
  pr_fs_t *fs = NULL;
  fsopendir_t *fsod = NULL, *fsodi = NULL;
  pool *fsod_pool = NULL;
  DIR *ret = NULL;

  if (!strchr(path, '/')) {
    fs = fs_cwd;

  } else {
    char buf[MAXPATHLEN + 1] = {'\0'};

    if (pr_fs_resolve_partial(path, buf, MAXPATHLEN, FSIO_DIR_OPENDIR) == -1)
      return NULL;

    fs = fs_lookup_dir(buf, FSIO_DIR_OPENDIR);
  }

  if (fs->opendir) {
    log_debug(DEBUG9, "FS: using %s opendir()",
      fs->opendir == sys_opendir ? "system" : fs->fs_name);
    ret = fs->opendir(fs, path);

  } else {
    errno = EPERM;
    return NULL;
  }

  if (!ret)
    return NULL;

  /* Cache it here */
  fs_cache_dir = ret;
  fs_cache_fsdir = fs;

  fsod_pool = make_sub_pool(permanent_pool);
  fsod = pcalloc(fsod_pool, sizeof(fsopendir_t));

  if (!fsod) {

    if (fs->closedir) {
      fs->closedir(fs, ret);
      errno = ENOMEM;
      return NULL;

    } else {
      errno = EPERM;
      return NULL;
    }
  }

  fsod->pool = fsod_pool;
  fsod->dir = ret;
  fsod->fsdir = fs;
  fsod->next = NULL;
  fsod->prev = NULL;

  if (fsopendir_list) {

    /* find the end of the fsopendir list */
    fsodi = fsopendir_list;
    while (fsodi->next)
      fsodi = fsodi->next;

    fsod->next = NULL;
    fsod->prev = fsodi;
    fsodi->next = fsod;

  } else

    /* This fsopendir _becomes_ the start of the fsopendir list */
    fsopendir_list = fsod;

  return ret;
}

static pr_fs_t *find_opendir(void *dir, int closing) {
  pr_fs_t *fs = NULL;

  if (dir == fs_cache_dir) {
    fs = fs_cache_fsdir;
    if (closing) {
      fs_cache_dir = NULL;
      fs_cache_fsdir = NULL;
    }

  } else {
    fsopendir_t *fsod;

    if (fsopendir_list) {
      for (fsod = fsopendir_list; fsod; fsod = fsod->next) {
        if (fsod->dir && fsod->dir == dir) {
          fs = fsod->fsdir;
          break;
        }
      }

      if (closing && fsod) {
        if (fsod->prev)
          fsod->prev->next = fsod->next;

        if (fsod->next)
          fsod->next->prev = fsod->prev;

        destroy_pool(fsod->pool);;
      }
    }
  }

  return fs;
}

int pr_fsio_closedir(void *dir) {
  pr_fs_t *fs = find_opendir(dir, TRUE);

  if (!fs)
    return -1;

  if (!fs->closedir) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s closedir()",
    fs->closedir == sys_closedir ? "system" : fs->fs_name);
  return fs->closedir(fs, dir);
}

struct dirent *pr_fsio_readdir(void *dir) {
  pr_fs_t *fs = find_opendir(dir, FALSE);

  if (!fs)
    return NULL;

  if (!fs->readdir) {
    errno = EPERM;
    return NULL;
  }

  log_debug(DEBUG9, "FS: using %s readdir()",
    fs->readdir == sys_readdir ? "system" : fs->fs_name);
  return fs->readdir(fs, dir);
}

int pr_fsio_mkdir(const char *path, mode_t mode) {
  pr_fs_t *fs = fs_lookup_dir(path, FSIO_DIR_MKDIR);

  if (!fs->mkdir) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s mkdir()",
    fs->mkdir == sys_mkdir ? "system" : fs->fs_name);
  return fs->mkdir(fs, path, mode);
}

int pr_fsio_rmdir(const char *path) {
  pr_fs_t *fs = fs_lookup_dir(path, FSIO_DIR_RMDIR);

  if (!fs->rmdir) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s rmdir()",
    fs->rmdir == sys_rmdir ? "system" : fs->fs_name);
  return fs->rmdir(fs, path);
}

int pr_fsio_stat_canon(const char *path, struct stat *sbuf) {
  pr_fs_t *fs = fs_lookup_file_canon(path, NULL, FSIO_FILE_STAT);

  if (!fs->stat) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s stat()",
    fs->stat == sys_stat ? "system" : fs->fs_name);
  return fs_cache_stat(fs, path, sbuf);
}

int pr_fsio_stat(const char *path, struct stat *sbuf) {
  pr_fs_t *fs = fs_lookup_file(path, NULL, FSIO_FILE_STAT);

  if (!fs->stat) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s stat()",
    fs->stat == sys_stat ? "system" : fs->fs_name);
  return fs_cache_stat(fs, path, sbuf);
}

int pr_fsio_fstat(pr_fh_t *fh, struct stat *sbuf) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->fstat) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s fstat()",
    fh->fh_fs->fstat == sys_fstat ? "system" : fh->fh_fs->fs_name);
  return fh->fh_fs->fstat(fh, fh->fh_fd, sbuf);
}

int pr_fsio_lstat_canon(const char *path, struct stat *sbuf) {
  pr_fs_t *fs = fs_lookup_file_canon(path, NULL, FSIO_FILE_LSTAT);

  if (!fs->lstat) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s lstat()",
    fs->lstat == sys_lstat ? "system" : fs->fs_name);
  return fs_cache_lstat(fs, path, sbuf);
}

int pr_fsio_lstat(const char *path, struct stat *sbuf) {
  pr_fs_t *fs = fs_lookup_file(path, NULL, FSIO_FILE_LSTAT);

  if (!fs->lstat) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s lstat()",
    fs->lstat == sys_lstat ? "system" : fs->fs_name);
  return fs_cache_lstat(fs, path, sbuf);
}

int pr_fsio_readlink_canon(const char *path, char *buf, size_t buflen) {
  pr_fs_t *fs = fs_lookup_file_canon(path, NULL, FSIO_FILE_READLINK);

  if (!fs->readlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s readlink()",
    fs->readlink == sys_readlink ? "system" : fs->fs_name);
  return fs->readlink(fs, path, buf, buflen);
}

int pr_fsio_readlink(const char *path, char *buf, size_t buflen) {
  pr_fs_t *fs = fs_lookup_file(path, NULL, FSIO_FILE_READLINK);

  if (!fs->readlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s readlink()",
    fs->readlink == sys_readlink ? "system" : fs->fs_name);
  return fs->readlink(fs, path, buf, buflen);
}

/* pr_fs_glob() is just a wrapper for glob(3), setting the various gl_
 * callbacks to our fs functions.
 */
int pr_fs_glob(const char *pattern, int flags,
    int (*errfunc)(const char *, int), glob_t *pglob) {

  if (pglob) {
    flags |= GLOB_ALTDIRFUNC;

    pglob->gl_closedir = (void (*)(void *)) pr_fsio_closedir;
    pglob->gl_readdir = pr_fsio_readdir;
    pglob->gl_opendir = pr_fsio_opendir;
    pglob->gl_lstat = pr_fsio_lstat;
    pglob->gl_stat = pr_fsio_stat;
  }

  return glob(pattern, flags, errfunc, pglob);
}

void pr_fs_globfree(glob_t *pglob) {
  globfree(pglob);
}

int pr_fsio_rename_canon(const char *rfrom, const char *rto) {
  pr_fs_t *fs = fs_lookup_file_canon(rfrom, NULL, FSIO_FILE_RENAME);

  if (fs != fs_lookup_file_canon(rto, NULL, FSIO_FILE_RENAME)) {
    errno = EXDEV;
    return -1;
  }

  if (!fs->rename) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s rename()",
    fs->rename == sys_rename ? "system" : fs->fs_name);
  return fs->rename(fs, rfrom, rto);
}

int pr_fsio_rename(const char *rnfm, const char *rnto) {
  pr_fs_t *fs = fs_lookup_file(rnfm, NULL, FSIO_FILE_RENAME);

  if (fs != fs_lookup_file(rnto, NULL, FSIO_FILE_RENAME)) {
    errno = EXDEV;
    return -1;
  }

  if (!fs->rename) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s rename()",
    fs->rename == sys_rename ? "system" : fs->fs_name);
  return fs->rename(fs, rnfm, rnto);
}

int pr_fsio_unlink_canon(const char *name) {
  pr_fs_t *fs = fs_lookup_file_canon(name, NULL, FSIO_FILE_UNLINK);

  if (!fs->unlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s unlink()",
    fs->unlink == sys_unlink ? "system" : fs->fs_name);
  return fs->unlink(fs, name);
}
	
int pr_fsio_unlink(const char *name) {
  pr_fs_t *fs = fs_lookup_file(name, NULL, FSIO_FILE_UNLINK);

  if (!fs->unlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s unlink()",
    fs->unlink == sys_unlink ? "system" : fs->fs_name);
  return fs->unlink(fs, name);
}

pr_fh_t *pr_fsio_open_canon(const char *name, int flags) {
  char *deref = NULL;
  pool *tmp_pool = NULL;
  pr_fh_t *fh = NULL;

  pr_fs_t *fs = fs_lookup_file_canon(name, &deref, FSIO_FILE_OPEN);

  if (!fs->open) {
    errno = EPERM;
    return NULL;
  }

  /* Allocate a filehandle. */
  tmp_pool = make_sub_pool(fs->fs_pool);
  fh = pcalloc(tmp_pool, sizeof(pr_fh_t));
  fh->fh_pool = tmp_pool;
  fh->fh_path = pstrdup(fh->fh_pool, name);
  fh->fh_fd = -1;
  fh->fh_buf = NULL;

  log_debug(DEBUG9, "FS: using %s open()",
    fs->open == sys_open ? "system" : fs->fs_name);
  fh->fh_fd = fs->open(fs, deref, flags);

  if (fh->fh_fd == -1) {
    destroy_pool(fh->fh_pool);
    return NULL;
  }

  fh->fh_fs = fs;
  return fh;
}

pr_fh_t *pr_fsio_open(const char *name, int flags) {
  pool *tmp_pool = NULL;
  pr_fh_t *fh = NULL;
  pr_fs_t *fs = fs_lookup_file(name, NULL, FSIO_FILE_OPEN);

  if (!fs->open) {
    errno = EPERM;
    return NULL;
  }

  /* Allocate a filehandle. */
  tmp_pool = make_sub_pool(fs->fs_pool);
  fh = pcalloc(tmp_pool, sizeof(pr_fh_t));
  fh->fh_pool = tmp_pool;
  fh->fh_path = pstrdup(fh->fh_pool, name);
  fh->fh_fd = -1;
  fh->fh_buf = NULL;

  log_debug(DEBUG9, "FS: using %s open()",
    fs->open == sys_open ? "system" : fs->fs_name);
  fh->fh_fd = fs->open(fs, name, flags);

  if (fh->fh_fd == -1) {
    destroy_pool(fh->fh_pool);
    return NULL;
  }

  fh->fh_fs = fs;
  return fh;
}

pr_fh_t *pr_fsio_creat_canon(const char *name, mode_t mode) {
  char *deref = NULL;
  pool *tmp_pool = NULL;
  pr_fh_t *fh = NULL;
  pr_fs_t *fs = fs_lookup_file_canon(name, &deref, FSIO_FILE_CREAT);

  if (!fs->creat) {
    errno = EPERM;
    return NULL;
  }

  /* Allocate a filehandle. */
  tmp_pool = make_sub_pool(fs->fs_pool);
  fh = pcalloc(tmp_pool, sizeof(pr_fh_t));
  fh->fh_pool = tmp_pool;
  fh->fh_path = pstrdup(fh->fh_pool, name);
  fh->fh_fd = -1;
  fh->fh_buf = NULL;

  log_debug(DEBUG9, "FS: using %s creat()",
    fs->creat == sys_creat ? "system" : fs->fs_name);
  fh->fh_fd = fs->creat(fs, deref, mode);

  if (fh->fh_fd == -1) {
    destroy_pool(fh->fh_pool);
    return NULL;
  }

  fh->fh_fs = fs;
  return fh;
}

pr_fh_t *pr_fsio_creat(const char *name, mode_t mode) {
  pool *tmp_pool = NULL;
  pr_fh_t *fh = NULL;
  pr_fs_t *fs = fs_lookup_file(name, NULL, FSIO_FILE_CREAT);

  if (!fs->creat) {
    errno = EPERM;
    return NULL;
  }

  /* Allocate a filehandle. */
  tmp_pool = make_sub_pool(fs->fs_pool);
  fh = pcalloc(tmp_pool, sizeof(pr_fh_t));
  fh->fh_pool = tmp_pool;
  fh->fh_path = pstrdup(fh->fh_pool, name);
  fh->fh_fd = -1;
  fh->fh_buf = NULL;

  log_debug(DEBUG9, "FS: using %s creat()",
    fs->creat == sys_creat ? "system" : fs->fs_name);
  fh->fh_fd = fs->creat(fs, name, mode);

  if (fh->fh_fd == -1) {
    destroy_pool(fh->fh_pool);
    return NULL;
  }

  fh->fh_fs = fs;
  return fh;
}

int pr_fsio_close(pr_fh_t *fh) {
  int res = 0;

  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->close) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s close()",
    fh->fh_fs->close == sys_close ? "system" : fh->fh_fs->fs_name);
  res = fh->fh_fs->close(fh, fh->fh_fd);

  destroy_pool(fh->fh_pool);
  return res;
}

int pr_fsio_read(pr_fh_t *fh, char *buf, size_t size) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->read) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s read()",
    fh->fh_fs->read == sys_read ? "system" : fh->fh_fs->fs_name);
  return fh->fh_fs->read(fh, fh->fh_fd, buf, size);
}

int pr_fsio_write(pr_fh_t *fh, const char *buf, size_t size) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->write) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s write()",
    fh->fh_fs->write == sys_write ? "system" : fh->fh_fs->fs_name);
  return fh->fh_fs->write(fh, fh->fh_fd, buf, size);
}

off_t pr_fsio_lseek(pr_fh_t *fh, off_t offset, int whence) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->lseek) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s lseek()",
    fh->fh_fs->lseek == sys_lseek ? "system" : fh->fh_fs->fs_name);
  return fh->fh_fs->lseek(fh, fh->fh_fd, offset, whence);
}

int pr_fsio_link_canon(const char *lfrom, const char *lto) {
  pr_fs_t *fs = fs_lookup_file_canon(lfrom, NULL, FSIO_FILE_LINK);

  if (fs != fs_lookup_file_canon(lto, NULL, FSIO_FILE_LINK)) {
    errno = EXDEV;
    return -1;
  }

  if (!fs->link) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s link()",
    fs->link == sys_link ? "system" : fs->fs_name);
  return fs->link(fs, lfrom, lto);
}

int pr_fsio_link(const char *lfrom, const char *lto) {
  pr_fs_t *fs = fs_lookup_file(lfrom, NULL, FSIO_FILE_LINK);

  if (fs != fs_lookup_file(lto, NULL, FSIO_FILE_LINK)) {
    errno = EXDEV;
    return -1;
  }

  if (!fs->link) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s link()",
    fs->link == sys_link ? "system" : fs->fs_name);
  return fs->link(fs, lfrom, lto);
}

int pr_fsio_symlink_canon(const char *lfrom, const char *lto) {
  pr_fs_t *fs = fs_lookup_file_canon(lto, NULL, FSIO_FILE_SYMLINK);

  if (!fs->symlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s symlink()",
    fs->symlink == sys_symlink ? "system" : fs->fs_name);
  return fs->symlink(fs, lfrom, lto);
}

int pr_fsio_symlink(const char *lfrom, const char *lto) {
  pr_fs_t *fs = fs_lookup_file(lto, NULL, FSIO_FILE_SYMLINK);

  if (!fs->symlink) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s symlink()",
    fs->symlink == sys_symlink ? "system" : fs->fs_name);
  return fs->symlink(fs, lfrom, lto);
}

int pr_fsio_ftruncate(pr_fh_t *fh, off_t len) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  if (!fh->fh_fs->ftruncate) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s ftruncate()",
    fh->fh_fs->ftruncate == sys_ftruncate ? "system" : fh->fh_fs->fs_name);
  return fh->fh_fs->ftruncate(fh, fh->fh_fd, len);
}

int pr_fsio_truncate_canon(const char *path, off_t len) {
  pr_fs_t *fs = fs_lookup_file_canon(path, NULL, FSIO_FILE_TRUNC);

  if (!fs->truncate) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s truncate()",
    fs->truncate == sys_truncate ? "system" : fs->fs_name);
  return fs->truncate(fs, path, len);
}

int pr_fsio_truncate(const char *path, off_t len) {
  pr_fs_t *fs = fs_lookup_file(path, NULL, FSIO_FILE_TRUNC);

  if (!fs->truncate) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s truncate()",
    fs->truncate == sys_truncate ? "system" : fs->fs_name);
  return fs->truncate(fs, path, len);
}

int pr_fsio_chmod_canon(const char *name, mode_t mode) {
  char *deref = NULL;
  pr_fs_t *fs = fs_lookup_file_canon(name, &deref, FSIO_FILE_CHMOD);

  if (!fs->chmod) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s chmod()",
    fs->chmod == sys_chmod ? "system" : fs->fs_name);
  return fs->chmod(fs, deref, mode);
}

int pr_fsio_chmod(const char *name, mode_t mode) {
  pr_fs_t *fs = fs_lookup_file(name, NULL, FSIO_FILE_CHMOD);

  if (!fs->chmod) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s chmod()",
    fs->chmod == sys_chmod ? "system" : fs->fs_name);
  return fs->chmod(fs, name, mode);
}

int pr_fsio_chown_canon(const char *name, uid_t uid, gid_t gid) {
  pr_fs_t *fs = fs_lookup_file_canon(name, NULL, FSIO_FILE_CHOWN);

  if (!fs->chown) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s chown()",
    fs->chown == sys_chown ? "system" : fs->fs_name);
  return fs->chown(fs, name, uid, gid);
}

int pr_fsio_chown(const char *name, uid_t uid, gid_t gid) {
  pr_fs_t *fs = fs_lookup_file(name, NULL, FSIO_FILE_CHOWN);

  if (!fs->chown) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s chown()",
    fs->chown == sys_chown ? "system" : fs->fs_name);
  return fs->chown(fs, name, uid, gid);
}

/* If the wrapped chroot() function suceeds (eg returns 0), then all
 * pr_fs_ts currently registered in the fs_map will have their paths
 * rewritten to reflect the new root.
 */
int pr_fsio_chroot(const char *path) {
  int res = 0;
  pr_fs_t *fs = fs_lookup_dir(path, FSIO_DIR_CHROOT);

  if (!fs->chroot) {
    errno = EPERM;
    return -1;
  }

  log_debug(DEBUG9, "FS: using %s chroot()",
    fs->chroot == sys_chroot ? "system" : fs->fs_name);

  if ((res = fs->chroot(fs, path)) == 0) {

    /* The fs_t's in fs_map need to be readjusted to the new root.
     * The pr_fs_t returned by fs_lookup_dir() will be the new root_fs,
     * and all others will re-inserted and resorted into a new map.
     */

    register unsigned int i = 0;
    pool *map_pool = make_sub_pool(permanent_pool);
    array_header *new_map = make_array(map_pool, 0, sizeof(pr_fs_t *));
    pr_fs_t **fs_objs = NULL;

    if (fs_map)
      fs_objs = (pr_fs_t **) fs_map->elts;

    /* Adjust the new root fs */
    if (!strncmp(fs->fs_path, path, strlen(path))) {
      memmove(fs->fs_path, fs->fs_path + strlen(path),
        strlen(fs->fs_path) - strlen(path) + 1);
    }

    *((pr_fs_t **) push_array(new_map)) = fs;
    root_fs = fs;

    for (i = 1; i < (fs_map ? fs_map->nelts : 0); i++) {
      pr_fs_t *tmpfs = fs_objs[i];

      /* The memory for this field has already been allocated, so futzing
       * with it like this should be fine.  Watch out for any paths that
       * may be different, e.g. added manually, not through pr_register_fs().
       * Any absolute paths that are outside of the chroot path are discarded.
       * Deferred-resolution paths (eg "~" paths) and relative paths are kept.
       */
      if (!strncmp(tmpfs->fs_path, path, strlen(path))) {
        memmove(tmpfs->fs_path, tmpfs->fs_path + strlen(path),
          strlen(tmpfs->fs_path) - strlen(path) + 1);
        *((pr_fs_t **) push_array(new_map)) = tmpfs;

      } else if ((tmpfs->fs_path)[0] != '/') {
        *((pr_fs_t **) push_array(new_map)) = tmpfs;
      }
    }

    /* Sort the new map */
    qsort(new_map->elts, new_map->nelts, sizeof(pr_fs_t *), fs_cmp);

    /* Destroy the old map */
    if (fs_map)
      destroy_pool(fs_map->pool);

    fs_map = new_map;
    chk_fs_map = TRUE;
  }

  return res;
}

char *pr_fsio_gets(char *buf, size_t size, pr_fh_t *fh) {
  char *bp = NULL;
  int toread = 0;
  pr_buffer_t *pbuf = NULL;

  if (!buf || !fh || size <= 0) {
    errno = EINVAL;
    return NULL;
  }

  if (!fh->fh_buf) {
    fh->fh_buf = pcalloc(fh->fh_pool, sizeof(pr_buffer_t));
    fh->fh_buf->buf = fh->fh_buf->current = pcalloc(fh->fh_pool,
      PR_TUNABLE_BUFFER_SIZE);
    fh->fh_buf->remaining = fh->fh_buf->buflen = PR_TUNABLE_BUFFER_SIZE;
  }

  pbuf = fh->fh_buf;
  bp = buf;

  while (size) {
    if (!pbuf->current ||
        pbuf->remaining == pbuf->buflen) { /* empty buffer */

      toread = pr_fsio_read(fh, pbuf->buf,
        size < pbuf->buflen ? size : pbuf->buflen);

      if (toread <= 0) {
        if (bp != buf) {
          *bp = '\0';
          return buf;

        } else
          return NULL;
      }

      pbuf->remaining = pbuf->buflen - toread;
      pbuf->current = pbuf->buf;

    } else
      toread = pbuf->buflen - pbuf->remaining;

    while (size && *pbuf->current != '\n' && toread--) {
      *bp++ = *pbuf->current++;
      size--;
      pbuf->remaining++;
    }

    if (size && toread && *pbuf->current == '\n') {
      size--; toread--;
      *bp++ = *pbuf->current++;
      pbuf->remaining++;
      break;
    }

    if (!toread)
      pbuf->current = NULL;
  }

  *bp = '\0';
  return buf;
}

/* pr_fsio_getline() is an fgets() with backslash-newline stripping, copied from
 * Wietse Venema's tcpwrapppers-7.6 code.  The extra *lineno argument is
 * needed, at the moment, to properly track which line of the configuration
 * file is being read in, so that errors can be reported with line numbers
 * correctly.
 */
char *pr_fsio_getline(char *buf, int buflen, pr_fh_t *fh,
    unsigned int *lineno) {
  int inlen;
  char *start = buf;

  while (pr_fsio_gets(buf, buflen, fh)) {

    /* while() loops should always handle signals. */
    pr_signals_handle();

    inlen = strlen(buf);

    if (inlen >= 1 && buf[inlen - 1] == '\n') {
      (*lineno)++;

      if (inlen >= 2 && buf[inlen - 2] == '\\') {
        inlen -= 2;

      } else
        return start;
    }

    /* Be careful of reading too much. */
    if (buflen - inlen == 0)
      return buf;

    buf += inlen;
    buflen -= inlen;
    buf[0] = 0;
  }

  return (buf > start ? start : 0);
}

#if defined(HAVE_SYS_STATVFS_H) || defined(HAVE_SYS_VFS_H) || defined(HAVE_STATFS)

/* Simple multiplication and division doesn't work with very large
 * filesystems (overflows 32 bits).  This code should handle it.
 */
static off_t calc_fs_size(size_t blocks, size_t bsize) {
  off_t bl_lo, bl_hi;
  off_t res_lo, res_hi, tmp;

  bl_lo = blocks & 0x0000ffff;
  bl_hi = blocks & 0xffff0000;

  tmp = (bl_hi >> 16) * bsize;
  res_hi = tmp & 0xffff0000;
  res_lo = (tmp & 0x0000ffff) << 16;
  res_lo += bl_lo * bsize;

  if (res_hi & 0xfc000000)
    /* Overflow */
    return 0;

  return (res_lo >> 10) | (res_hi << 6);
}

off_t pr_fs_getsize(char *path) {
# if defined(HAVE_SYS_STATVFS_H)

#  if _FILE_OFFSET_BITS == 64 && defined(SOLARIS2) && \
   !defined(SOLARIS2_5_1) && !defined(SOLARIS2_6) && !defined(SOLARIS2_7)
  /* Note: somewhere along the way, Sun decided that the prototype for
   * its statvfs64(2) function would include a statvfs64_t rather than
   * struct statvfs64.  In 2.6 and 2.7, it's struct statvfs64, and
   * in 8, 9 it's statvfs64_t.  This should silence compiler warnings.
   * (The statvfs_t will be redefined to a statvfs64_t as appropriate on
   * LFS systems).
   */
  statvfs_t fs;
#  else
  struct statvfs fs;
#  endif /* LFS && !Solaris 2.5.1 && !Solaris 2.6 && !Solaris 2.7 */

  if (statvfs(path, &fs) != 0)
    return 0;

  return calc_fs_size(fs.f_bavail, fs.f_frsize);

# elif defined(HAVE_SYS_VFS_H)
  struct statfs fs;

  if (statfs(path, &fs) != 0)
    return 0;

  return calc_fs_size(fs.f_bavail, fs.f_bsize);
# elif defined(HAVE_STATFS)
  struct statfs fs;

  if (statfs(path, &fs) != 0)
    return 0;

  return calc_fs_size(fs.f_bavail, fs.f_bsize);
# endif /* !HAVE_STATFS && !HAVE_SYS_STATVFS && !HAVE_SYS_VFS */
}
#endif /* !HAVE_STATFS && !HAVE_SYS_STATVFS && !HAVE_SYS_VFS */

int pr_fsio_puts(const char *buf, pr_fh_t *fh) {
  if (!fh) {
    errno = EINVAL;
    return -1;
  }

  return pr_fsio_write(fh, buf, strlen(buf));
}

void pr_resolve_fs_map(void) {
  register unsigned int i = 0;

  if (!fs_map)
    return;

  for (i = 0; i < fs_map->nelts; i++) {
    char *newpath = NULL;
    unsigned char add_slash = FALSE;
    pr_fs_t *tmpfs = ((pr_fs_t **) fs_map->elts)[i];

    /* Skip if this fs is the root fs. */
    if (tmpfs == root_fs)
      continue;

    /* Note that dir_realpath() does _not_ handle "../blah" paths
     * well, so...at least for now, hope that such paths are screened
     * by the code adding such paths into the fs_map.  Check for
     * a trailing slash in the unadjusted path, so that I know if I need
     * to re-add that slash to the adjusted path -- these trailing slashes
     * are important!
     */
    if ((strcmp(tmpfs->fs_path, "/") != 0 &&
        (tmpfs->fs_path)[strlen(tmpfs->fs_path) - 1] == '/'))
      add_slash = TRUE;

    newpath = dir_realpath(tmpfs->fs_pool, tmpfs->fs_path);

    if (add_slash)
      newpath = pstrcat(tmpfs->fs_pool, newpath, "/", NULL);

    /* Note that this does cause a slightly larger memory allocation from
     * the pr_fs_t's pool, as the original path value was also allocated
     * from that pool, and that original pointer is being overwritten.
     * However, as this function is only called once, and that pool
     * is freed later, I think this may be acceptable.
     */
    tmpfs->fs_path = newpath;
  }

  /* Resort the map */
  qsort(fs_map->elts, fs_map->nelts, sizeof(pr_fs_t *), fs_cmp);

  return;
}

int pr_init_fs(void) {
  char cwdbuf[MAXPATHLEN + 1] = {'\0'};

  /* Establish the default pr_fs_t that will handle any path */
  if ((root_fs = pr_create_fs(permanent_pool, "system")) == NULL) {

    /* Do not insert this fs into the FS map.  This will allow other
     * modules to insert filesystems at "/", if they want.
     */
    log_pri(LOG_ERR, "error: unable to initialize default fs");
    exit(1);
  }

  root_fs->fs_path = pstrdup(root_fs->fs_pool, "/");

  if (getcwd(cwdbuf, MAXPATHLEN)) {
    pr_fs_setcwd(cwdbuf);

  } else {
    pr_fsio_chdir("/", FALSE);
    pr_fs_setcwd("/");
  }

  return 0;
}
