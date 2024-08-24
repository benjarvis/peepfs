#define FUSE_USE_VERSION 32

#include <stdio.h>
#include <stdarg.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fuse3/fuse.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "peepfs_archive.h"
#include "peepfs_cache.h"

#define MIN(x,y) ((x) < (y) ? (x) : (y))

/* Config parameters passed in from user via main() */
typedef struct peepfs_params {
    char        base[PATH_MAX];
    char        magic_suffix[NAME_MAX];
    int         magic_suffix_len;
    int64_t     max_cache_entries;
    int64_t     grace;
} peepfs_params_t;

/* 
 * Global (inter-thread) context for one mount
 * Mostly only used to make per-thread contexts
 */

typedef struct peepfs_global {
    peepfs_params_t *params;
    peepfs_cache_t  *cache;
    pthread_key_t    key;
} peepfs_global_t;

/* Per-thread context for one mount */
typedef struct peepfs_ctx {
    peepfs_params_t    *params;
    peepfs_cache_t     *cache;
    char                peepname[PATH_MAX];
    char                fullpath[PATH_MAX];
    char                old_fullpath[PATH_MAX];
    char                new_fullpath[PATH_MAX];
    char                archivepath[PATH_MAX];
    char                old_archivepath[PATH_MAX];
    char                new_archivepath[PATH_MAX];
} peepfs_ctx_t;

/* Cookie representing an open file 
 * for real files, we just hold a file descriptor
 * and proxy VOPs.
 * for archive files, we read the whole file into
 * memory at open() and then use it to serve read()
 */

typedef struct peepfs_cookie {
    int                     fd;
    peepfs_archive_t       *archive;
    peepfs_archive_entry_t  entry;
    peepfs_archive_file_t  *file;
} peepfs_cookie_t;

peepfs_params_t PeepParams;

static inline void
peepfs_panic(const char *fmt, ...)
{
    va_list argp;

    va_start(argp, fmt);

    fprintf(stderr, "[PANIC] ");
    vfprintf(stderr, fmt, argp);
    fprintf(stderr, "\n");

    va_end(argp);

    abort();
}

int PeepDebug = 0;

static inline void
peepfs_debug(const char *fmt, ...)
{
    if (PeepDebug) {
        va_list argp;

        va_start(argp, fmt);

        fprintf(stderr, "[Debug] ");
        vfprintf(stderr, fmt, argp);
        fprintf(stderr, "\n");
    
        va_end(argp);
    }

}

/* Generate a synthetic inode number from a real inode number
 * and a child index
 */

static inline uint64_t
peepfs_compose_ino(uint64_t base, uint64_t rel)
{
    return (rel & 0xffffffffUL) | (base << 32);
}

/* Generate the base path from a path relative to our mountpoint */
static inline void
peepfs_compose_path(char *out, peepfs_ctx_t *ctx, const char *relpath)
{
    snprintf(out, PATH_MAX, "%s%s", ctx->params->base, relpath);
}

/* Return 0 iff 'fullpath' appears to point inside an archive 
 * If 0, then set archivepath to the path of the archive,
 * and relpath to the path of the file within that archive
 */

int
peepfs_static_archive_path(
    peepfs_ctx_t   *ctx,
    const char     *fullpath, 
    char           *archivepath, 
    const char    **relpath)
{
    const char *token = fullpath;
    int         error;
    struct stat st;

    peepfs_debug("peepfs_static_archive_path: fullpath %s", fullpath);
  
    *relpath     = NULL; 

    while (token) {

        token = strstr(token,ctx->params->magic_suffix);

        if (token) {
            snprintf(archivepath, token - fullpath + 1, "%s", fullpath);
        
            peepfs_debug("peepfs_static_archive_path: trying '%s' as archive path",
                archivepath);

            error = lstat(archivepath, &st);

            if (error == 0 && S_ISREG(st.st_mode)) {

                token += ctx->params->magic_suffix_len;

                while (*token == '/') ++token;

                *relpath = token;

                return 0;
            }
 
            token++;
        }

        
    } 

    return -ENOENT;    
}


/*
 * Return 1 iff 'path/name' appears to be an archive file
 * and if so return the synthesized content dir name in 'out'
 */

int
peepfs_archive_ident(
    peepfs_ctx_t *ctx, const char *path, const char *name, char *out)
{
    int namelen = strlen(name);
    peepfs_archive_t *archive;

    if (namelen < 4) return 0;

    if (strcasecmp(name + namelen - 4, ".zip") == 0 ||
        strcasecmp(name + namelen - 4, ".tar") == 0 ||
        strcasecmp(name + namelen - 7, ".tar.gz") == 0 ||
        strcasecmp(name + namelen - 8, ".tar.bz2") == 0 ||
        strcasecmp(name + namelen - 7, ".tar.xz") == 0 ||
        strcasecmp(name + namelen - 4, ".tgz") == 0 ||
        strcasecmp(name + namelen - 4, ".iso") == 0 ||
        strcasecmp(name + namelen - 4, ".rar") == 0 ||
        strcasecmp(name + namelen - 4, ".cab") == 0
    ) {
        char archpath[PATH_MAX+1];

        snprintf(archpath, sizeof(archpath), "%s/%s/%s", ctx->params->base, path, name);

        archive = peepfs_archive_open(archpath);

        if (archive) {
            peepfs_archive_close(archive);
            snprintf(out, NAME_MAX, "%s%s", name, ctx->params->magic_suffix);
            return 1;
        }
    }

    return 0;

}

/* Initialize the global FUSE context, peepfs_global_t */

void *
peepfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    struct fuse_context   *fuse_ctx = fuse_get_context();
    peepfs_global_t       *gl;

    gl = calloc(1,sizeof(peepfs_global_t));

    if (gl== NULL) {
        /* There is no graceful way to fail here */
        fprintf(stderr,"Failed to allocate memory\n");
        abort();
    }

    gl->params = (peepfs_params_t*)fuse_ctx->private_data;

    gl->cache = peepfs_cache_init(gl->params->max_cache_entries, gl->params->grace);

    pthread_key_create(&gl->key, free);

    return gl;
}

/* Destroy the global FUSE context, peepfs_global_t */
void 
peepfs_destroy(void *private_data)
{
    peepfs_global_t *gl = (peepfs_global_t*)private_data;

    peepfs_cache_free(gl->cache);

    free(private_data);
}

/* 
 * Get the thread-local mount context, peepfs_ctx_t,
 * Initialize it if needed
 *
 */

static inline peepfs_ctx_t *
peepfs_get_ctx()
{
    struct fuse_context *fuse_ctx = fuse_get_context();
    peepfs_global_t     *gl = (peepfs_global_t*)fuse_ctx->private_data;
    peepfs_ctx_t        *ctx;

    ctx = (peepfs_ctx_t*)pthread_getspecific(gl->key);

    if (ctx == NULL) {

        ctx = (peepfs_ctx_t*)calloc(1,sizeof(peepfs_ctx_t));

        pthread_setspecific(gl->key, (void*)ctx);
   
        ctx->params = gl->params;
        ctx->cache  = gl->cache;
    }


    return ctx;
}

int peepfs_getattr(
    const char*             path,
    struct stat*            stbuf,
    struct fuse_file_info *fi)
{
    peepfs_ctx_t           *ctx = peepfs_get_ctx();
    int                     error;
    const char             *relpath;
    peepfs_archive_t       *archive;
    peepfs_archive_entry_t  entry;

    peepfs_debug("peepfs_getattr: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(
        ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = lstat(ctx->fullpath, stbuf);

        if (error == 0) {
            return 0;
        } else {
            return -errno;
        }

    } else {

	peepfs_debug("peepfs_getattr: archivepath %s", ctx->archivepath);

        error = lstat(ctx->archivepath, stbuf);

        if (error) {
            return -errno;
        }

        if (relpath[0] == '\0') {

            stbuf->st_ino   = peepfs_compose_ino(stbuf->st_ino, 1);
            stbuf->st_mode &= ~S_IFMT;
            stbuf->st_mode |= S_IFDIR;
            stbuf->st_size  = 4096;
            stbuf->st_blocks = 1;
            stbuf->st_nlink  = 1;

            return 0;

        } else {

            error = peepfs_cache_get(ctx->cache, ctx->archivepath, relpath, &entry);

            if (error) {

                archive = peepfs_archive_open(ctx->archivepath);
 
                error = peepfs_archive_entry_open(archive, relpath, &entry);

                if (error == 0) {
                    peepfs_cache_insert(ctx->cache, 
                        ctx->archivepath, relpath, 0, &entry);
                }

                peepfs_archive_close(archive);
            }
    
            if (error == 0) {

                if (entry.flags & PEEPFS_FLAG_DIR) {
                    stbuf->st_mode &= ~S_IFMT;
                    stbuf->st_mode |= S_IFDIR;
                    stbuf->st_size  = 4096;
                    stbuf->st_blocks = 1;
                } else {
                    stbuf->st_mode &= ~S_IFMT;
                    stbuf->st_mode |= S_IFREG;
                    stbuf->st_size = entry.size;
                    stbuf->st_blocks = entry.size / 4096 + 1;
                }

                stbuf->st_ino = peepfs_compose_ino(stbuf->st_ino, 
                    entry.index + 2);
                stbuf->st_nlink  = 1;

            }

            if (error == 0) {
                return 0;
            } else {
                return -ENOENT;
            }
        }
    }
}

/* State tracking for iterating an archive file */
typedef struct peepfs_readdir_ctx {
    peepfs_archive_t   *archive;
    peepfs_cache_t     *cache;
    fuse_fill_dir_t     filler;
    void               *buf;
    const char         *archivepath;
    uint64_t            archive_id;
    const char         *relpath;
    int                 relpath_len;
    int                 scanning;
    uint64_t            zip_ino;
} peepfs_readdir_ctx_t;

/* Called for each entry in an archive we want to enumerate for readdir */
static inline int 
peepfs_readdir_callback(const char *input_name, peepfs_archive_entry_t *entry, void *arg)
{
    peepfs_readdir_ctx_t   *ctx = (peepfs_readdir_ctx_t*)arg;
    char                    buf[NAME_MAX], *name;
    struct stat             st;
    int                     name_len, error = 0;

    name_len = snprintf(buf, NAME_MAX, "%s", input_name);

    name = buf;

    peepfs_debug("peepfs_readdir_callback: name %s relpath '%s'", name, ctx->relpath);

    while (name[name_len-1] == '/') {
        name[name_len-1] = '\0';
        name_len--;
    }
    
    if (!ctx->scanning) {
        peepfs_cache_insert(ctx->cache, ctx->archivepath, name, ctx->archive_id, entry);
    }

    if (ctx->relpath_len) {
        if (strncmp(name, ctx->relpath, ctx->relpath_len) != 0) {
            peepfs_debug("peepfs_readdir_callback: doesn't match relpath, skipping...");
            goto out;
        }
    
        name += ctx->relpath_len;

        if (name[0] != '/') {
            goto out;
        }

        name++;
    }

    if (index(name, '/')) {
        peepfs_debug("peepfs_readdir_callback: relpath suffix is not a simple name, skipping...");
        goto out;
    }

    st.st_ino  = peepfs_compose_ino(ctx->zip_ino, entry->index + 2);

    st.st_mode = (entry->flags & PEEPFS_FLAG_DIR) ? S_IFDIR : S_IFREG;

    ctx->filler(ctx->buf, name, &st, 0, 0);

out:

    return error;
}

int peepfs_readdir(
    const char *            path,
    void*                   buf,
    fuse_fill_dir_t         filler,
    off_t                   offset,
    struct fuse_file_info*  fi,
    enum fuse_readdir_flags flags)
{
    peepfs_ctx_t            *ctx = peepfs_get_ctx();
    peepfs_cookie_t         *cookie = (peepfs_cookie_t*)fi->fh;
    peepfs_readdir_ctx_t     readdir_ctx; 
    DIR                     *dir;
    struct dirent           *dirent;
    const char             *relpath;
    int                     error;
    struct stat             st;

    peepfs_debug("peepfs_readdir: path %s offset %ld cookie %p", path, offset, cookie);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(
            ctx, ctx->fullpath, ctx->archivepath, &relpath);

    peepfs_debug("peepfs_readdir: archive error %d path %s relpath %s",
        error,
        error ? "-" : ctx->archivepath,
        error ? "-" : relpath);

    if (error) {

        dir = opendir(ctx->fullpath);

        if (dir == NULL) {
            return -errno;
        }

        filler(buf,".",     NULL, 0, 0);
        filler(buf,"..",    NULL, 0, 0);

        while ((dirent = readdir(dir)) != NULL) {

            st.st_ino = dirent->d_ino;
            switch (dirent->d_type) {
            case DT_BLK:
                st.st_mode = S_IFBLK;
                break;
            case DT_CHR:
                st.st_mode = S_IFCHR;
                break;
            case DT_DIR:
                st.st_mode = S_IFDIR;
                break;
            case DT_FIFO:
                st.st_mode = S_IFIFO;
                break;
            case DT_LNK:
                st.st_mode = S_IFLNK;
                break;
            case DT_REG:
                st.st_mode = S_IFREG;
                break;
            case DT_SOCK:
                st.st_mode = S_IFSOCK;
                break;
            default:
                st.st_mode = S_IFREG;
            }

            filler(buf, dirent->d_name, &st, 0, 0);

            if (peepfs_archive_ident(ctx, path, dirent->d_name, ctx->peepname)) {
                st.st_ino = peepfs_compose_ino(dirent->d_ino, 1);
                st.st_mode = S_IFDIR;
                filler(buf, ctx->peepname, &st, 0, 0);
            }
        }

        closedir(dir);

    } else {

        filler(buf,".",     NULL, 0, 0);
        filler(buf,"..",    NULL, 0, 0);

        error = lstat(ctx->archivepath, &st);

        readdir_ctx.filler      = filler;
        readdir_ctx.zip_ino     = st.st_ino;
        readdir_ctx.buf         = buf;
        readdir_ctx.archivepath = ctx->archivepath;
        readdir_ctx.relpath     = relpath;
        readdir_ctx.relpath_len = strlen(relpath);
        readdir_ctx.cache       = ctx->cache;
        readdir_ctx.scanning    = 1;

        error = peepfs_cache_scandir(ctx->cache, ctx->archivepath,
            peepfs_readdir_callback, &readdir_ctx);
   
        if (error) { 

            readdir_ctx.scanning    = 0;

            readdir_ctx.archive = peepfs_archive_open(ctx->archivepath);

            readdir_ctx.archive_id = peepfs_cache_insert(
                ctx->cache, ctx->archivepath, NULL, 0, NULL);

            if (readdir_ctx.archive) {
                peepfs_archive_enumerate(readdir_ctx.archive,
                    peepfs_readdir_callback, &readdir_ctx);

                peepfs_archive_close(readdir_ctx.archive);
            }
        }

    }

    return 0;
}


int peepfs_mkdir(
    const char *            path,
    mode_t                  mode)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_mkdir: path %s mode %u", path, mode);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = mkdir(ctx->fullpath, mode);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}

int peepfs_mknod(
    const char *            path,
    mode_t                  mode,
    dev_t                   dev)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_mknod: path %s mode %u decv %u", path, mode, dev);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = mknod(ctx->fullpath, mode, dev);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}


int peepfs_rmdir(
    const char *            path)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_rmdir: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(
        ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = rmdir(ctx->fullpath);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}


int peepfs_open(
    const char*             path,
    struct fuse_file_info*  fi)
{
    peepfs_ctx_t           *ctx = peepfs_get_ctx();
    peepfs_cookie_t        *cookie;
    int                     fd, error;
    const char             *relpath;

    peepfs_debug("peepfs_open: path %s cookie %p\n", path, (void*)fi->fh);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx,
        ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        fd = open(ctx->fullpath, fi->flags);

        if (fd >= 0) {

            cookie = (peepfs_cookie_t*)calloc(1, sizeof(peepfs_cookie_t));

            if (cookie == NULL) {
                peepfs_panic("Failed to allocate memory");
            }

            cookie->fd = fd;

            fi->fh = (int64_t)cookie;
            return 0;
        } else {
            return -errno;
        }
    } else {

        /* Don't allow writing inside archives */
        if (fi->flags & (O_CREAT|O_TRUNC|O_WRONLY|O_RDWR)) {
            return -EACCES;
        }

        cookie = (peepfs_cookie_t*)calloc(1, sizeof(peepfs_cookie_t));

        if (cookie == NULL) {
            peepfs_panic("Failed to allocate memory");
        }

        cookie->archive = peepfs_archive_open(ctx->archivepath);

        if (cookie->archive == NULL) {
            free(cookie);
            return -ENOENT;
        }

        error = peepfs_archive_entry_open(cookie->archive, relpath, &cookie->entry);

        if (error) {
            peepfs_archive_close(cookie->archive);
            free(cookie);
            return -ENOENT;
        }

        cookie->file = peepfs_archive_file_open(cookie->archive, &cookie->entry);
        
        if (cookie->file == NULL) {
            peepfs_archive_close(cookie->archive);
            free(cookie);
            return -ENOENT;
        }

        fi->fh = (int64_t)cookie;

        return 0;
    }
}

int peepfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    peepfs_ctx_t    *ctx = peepfs_get_ctx();
    peepfs_cookie_t *cookie;
    int              fd, error;
    const char      *relpath;

    peepfs_debug("peepfs_create: path %s mode %d\n", path, mode);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx,
        ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        fd = open(ctx->fullpath, fi->flags, mode);

        if (fd >= 0) {

            cookie = (peepfs_cookie_t*)calloc(1, sizeof(peepfs_cookie_t));

            if (cookie == NULL) {
                peepfs_panic("Failed to allocate memory");
            }

            cookie->fd = fd;

            fi->fh = (int64_t)cookie;
            return 0;

        } else {
            return -errno;
        }
    } else {
        return -EACCES;
    }
}

int peepfs_release(
    const char*             path,
    struct fuse_file_info*  fi)
{
    peepfs_cookie_t *cookie;

    peepfs_debug("peepfs_release: path %s", path);

    cookie = (peepfs_cookie_t*)fi->fh;

    if (cookie->file) {
        peepfs_archive_file_close(cookie->archive, cookie->file);
        peepfs_archive_close(cookie->archive);
    } else {
        close(cookie->fd);
    }

    free(cookie);

    return 0;
}


int peepfs_rename(
    const char*             oldpath,
    const char*             newpath,
    unsigned int 	    flags)
{

    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *old_relpath;
    const char     *new_relpath;

    peepfs_debug("peepfs_rename: old %s new %s", oldpath, newpath);

    peepfs_compose_path(ctx->old_fullpath, ctx, oldpath);
    peepfs_compose_path(ctx->new_fullpath, ctx, newpath);

    error = peepfs_static_archive_path(ctx,
        ctx->old_fullpath, ctx->old_archivepath, &old_relpath);

    if (error == 0) {
        return -EACCES;
    }

    error = peepfs_static_archive_path(ctx,
        ctx->new_fullpath, ctx->new_archivepath, &new_relpath);

    if (error == 0) {
        return -EACCES;
    }

    error = rename(ctx->old_fullpath, ctx->new_fullpath);

    if (error) {
        return -errno;
    } else {
        return 0;
    }

}


int peepfs_unlink(
    const char*             path)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_unlink: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = unlink(ctx->fullpath);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}


int peepfs_link(
    const char*             oldpath,
    const char*             newpath)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *old_relpath;
    const char     *new_relpath;

    peepfs_debug("peepfs_link: old %s new %s", oldpath, newpath);

    peepfs_compose_path(ctx->old_fullpath, ctx, oldpath);
    peepfs_compose_path(ctx->new_fullpath, ctx, newpath);

    error = peepfs_static_archive_path(ctx,
        ctx->old_fullpath, ctx->old_archivepath, &old_relpath);

    if (error == 0) {
        return -EACCES;
    }

    error = peepfs_static_archive_path(ctx,
        ctx->new_fullpath, ctx->new_archivepath, &new_relpath);

    if (error == 0) {
        return -EACCES;
    }

    error = link(ctx->old_fullpath, ctx->new_fullpath);

    if (error) {
        return -errno;
    } else {
        return 0;
    }

}


int peepfs_symlink(
    const char*             target,
    const char*             path)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_symlink: target %s path %s", target, path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = symlink(target, ctx->fullpath);

        if (error) {
            return -errno;
        } else { 
            return 0;
        }
    } else {
        return -EACCES;
    }
}


int peepfs_readlink(
    const char*             path,
    char*                   buffer,
    size_t                  bufferLen)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;
    
    peepfs_debug("peepfs_readlink: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = readlink(ctx->fullpath, buffer, bufferLen);

        if (error >= 0) {
            return 0;
        } else {
            return -errno;
        }

    } else {

        return -EACCES;
    }
}

int peepfs_utimens(
    const char *path,
    const struct timespec ts[2],
    struct fuse_file_info *fi)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;
    struct timeval  tv[2];

    peepfs_debug("peepfs_utime: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

	tv[0].tv_sec  = ts[0].tv_sec;
	tv[0].tv_usec = ts[0].tv_nsec / 1000;
	tv[1].tv_sec  = ts[1].tv_sec;
	tv[1].tv_usec = ts[1].tv_nsec / 1000;

        error = utimes(ctx->fullpath, tv);

        if (error) {
            return -errno;
	}

        return 0;

    } else {

        return -EACCES;
    }
}

int peepfs_chmod(
    const char*             path,
    mode_t                  mode,
    struct fuse_file_info  *fi)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_chmod: path %s mode %u", path, mode);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = chmod(ctx->fullpath, mode);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}


int peepfs_chown(
    const char*             path,
    uid_t                   uid,
    gid_t                   gid,
    struct fuse_file_info  *fi)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_chown: path %s uid %u gid %u", path, uid, gid);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = chown(ctx->fullpath, uid, gid);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        return -EACCES;
    }
}


int peepfs_access(
    const char*             path,
    int                     mode)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_access: path %s mode %u", path, mode);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = access(ctx->fullpath, mode);

        if (error) {
            return -errno;
        } else {
            return 0;
        }

    } else {

        peepfs_debug("peepfs_access: nested check archive %s",
            ctx->archivepath);

        if (mode & W_OK) {
            peepfs_debug("peepfs_access: suppressing write access inside archive");
            return -EACCES;
        }

        error = access(ctx->archivepath, R_OK);

        peepfs_debug("peepfs_access: nested access check returned error %d errno %d",
            error, errno);

        if (error) {
            return -errno;
        } else {
            return 0;
        }
    }
}

int peepfs_read(
    const char*             path,
    char*                   buf,
    size_t                  size,
    off_t                   offset,
    struct fuse_file_info*  fi)
{
    peepfs_cookie_t *cookie = (peepfs_cookie_t*)fi->fh;
    ssize_t len;

    peepfs_debug("peepfs_read: path %s offset %lu size %lu", path, offset, size);

    if (cookie == NULL) {
        peepfs_panic("peepfs_read: null cookie");
    }

    if (cookie->file) {

        len = peepfs_archive_file_read(
            cookie->archive, cookie->file, buf, offset, size);

    } else {

        len = pread(cookie->fd, buf, size, offset);

        if (len < 0) {
            return -errno;
        }
    }

    return len;
}


int peepfs_write(
    const char*             path,
    const char*             buf,
    size_t                  size,
    off_t                   offset,
    struct fuse_file_info*  fi)
{
    peepfs_cookie_t *cookie = (peepfs_cookie_t*)fi->fh;
    ssize_t len;

    peepfs_debug("peepfs_write: path %s offset %lu size %lu", path, offset, size);

    if (cookie->file) {
        return -ENOTSUP;
    }

    len = pwrite(cookie->fd, buf, size, offset);

    if (len < 0) {
        return -errno;
    }

    return len;
}


int peepfs_truncate(
    const char*             path,
    off_t                   size,
    struct fuse_file_info  *fi)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_truncate: path %s size %lu", path, size);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = truncate(ctx->fullpath, size);

        if (error) {
            return -errno;
        } else {
            return 0;
        }
    } else {
        return -EACCES;
    }
}


int peepfs_statfs(
    const char*             path,
    struct statvfs*         buf)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;

    peepfs_debug("peepfs_statfs: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = statvfs(ctx->fullpath, buf);

    if (error) {
        return -errno;
    }

    return 0;
}

int peepfs_listxattr(
    const char             *path,
    char                   *names,
    size_t                  max_names_len)
{
    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_listxattr: path %s", path);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {
        error = listxattr(ctx->fullpath, names, max_names_len);

        if (error) {
            return -errno;
        } else {
            return 0;
        }
    } else {
        return -ENOTSUP;
    }
}

#ifdef __APPLE__
int peepfs_getxattr(
    const char     *path, 
    const char     *name, 
    char           *value, 
    size_t          max_size, 
    uint32_t        options)
#else
int peepfs_getxattr(
    const char     *path, 
    const char     *name, 
    char           *value, 
    size_t          max_size)
#endif
{

    peepfs_ctx_t   *ctx = peepfs_get_ctx();
    int             error;
    const char     *relpath;

    peepfs_debug("peepfs_getxattr: path %s name %s", path, name);

    peepfs_compose_path(ctx->fullpath, ctx, path);

    error = peepfs_static_archive_path(ctx, ctx->fullpath, ctx->archivepath, &relpath);

    if (error) {

        error = getxattr(ctx->fullpath, name, value, max_size);

        if (error) {
            return -errno;
        } else {
            return 0;
        }
    } else {
        return -ENOTSUP;
    }
}

static struct fuse_operations peepfs_oper = {
    .init       = peepfs_init,
    .destroy    = peepfs_destroy,
    .getattr    = peepfs_getattr,
    .readdir    = peepfs_readdir,
    .mkdir      = peepfs_mkdir,
    .mknod      = peepfs_mknod,
    .rmdir      = peepfs_rmdir,
    .open       = peepfs_open,
    .create     = peepfs_create,
    .release    = peepfs_release,
    .rename     = peepfs_rename,
    .read       = peepfs_read,
    .write      = peepfs_write,
    .truncate   = peepfs_truncate,
    .utimens    = peepfs_utimens,
    .unlink     = peepfs_unlink,
    .link       = peepfs_link,
    .symlink    = peepfs_symlink,
    .readlink   = peepfs_readlink,
    .chmod      = peepfs_chmod,
    .chown      = peepfs_chown,
    .access     = peepfs_access,
    .statfs     = peepfs_statfs,
    .listxattr  = peepfs_listxattr,
    .getxattr   = peepfs_getxattr
};

void help()
{
    fprintf(stderr,"peepfs [-f] [-d] [-g <cache grace in seconds>] [-n <max cache entries] [-m magic_suffix] <peepfs mountpoint> <basefs mountpoint>\n");
}

int 
main(int argc,char **argv) 
{
    char*                   base = NULL;
    int                     error = 0, len;
    struct stat             st;
    char                   *fuse_argv[16];
    int                     fuse_argc = 0, option_index, c;

    fuse_argv[fuse_argc++] = argv[0];
    //fuse_argv[fuse_argc++] = "-o";
    //fuse_argv[fuse_argc++] = "use_ino,allow_other,default_permissions";

    PeepParams.max_cache_entries = 1024*1024;
    PeepParams.grace = 10;
    snprintf(PeepParams.magic_suffix, NAME_MAX, "%s", ".peep");

    while (1) {

        static struct option long_options[] = {
            { "foreground", no_argument,    0,  'f' },
            { "debug",      no_argument,    0,  'd' },
            { "help",       no_argument,    0,  'h' },
            { "magic_suffix", required_argument, 0, 'm' },
            { "cache_size", required_argument, 0, 'n' },
            { "cache_grace", required_argument, 0, 'g' },
            { NULL,         0,              0,  0   }
        };

        option_index = 0;

        c = getopt_long(argc, argv, "dfg:hn:V", long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            if (long_options[option_index].flag != 0) {
                break;
            }

            break;

        case 'd':
            fuse_argv[fuse_argc++] = "-d";
            PeepDebug = 1;
            break;

        case 'f':
            fuse_argv[fuse_argc++] = "-f";
            break;

        case 'g':
            PeepParams.grace = strtoul(optarg, NULL, 10);
            break;
            
        case 'h':
            help();
            exit(0);

        case 'm':
            snprintf(PeepParams.magic_suffix, NAME_MAX, ".%s", optarg);
            break;

        case 'n':
            PeepParams.max_cache_entries = strtoul(optarg, NULL, 10);
            break;

        case 'V':
            printf("peepfs version %s\n", PEEPFS_VERSION_STR);
            exit(0);
            break;

        case '?':
            help();
            exit(0);
            break;

        default:
            abort();
        }
    }

    if (optind >= argc) {
        help();
        return 1;
    }

    fuse_argv[fuse_argc++] = argv[optind];

    optind++;

    if (optind >= argc) {
        help();
        return 1;
    }

    base = argv[optind];

    /* Don't want trailing slashes on the base path */
    len = strlen(base);

    while (base[len-1] == '/') {
        base[len-1] = '\0';
        len--;
    }

    /* Make sure the base we were given looks legit */
    error = lstat(base, &st);

    if (error) {
        fprintf(stderr,"Failed to open base directory: %s\n", strerror(errno));
        exit(1);
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr,"Base '%s' is not a directory\n", base);
        exit(1);
    }

    /* 
     * Populate the config params global, which we'll consume
     * from peepfs_init()
     */

    snprintf(PeepParams.base, PATH_MAX, "%s", base);

    PeepParams.magic_suffix_len = strlen(PeepParams.magic_suffix);

    /* Start fusing */
    fuse_main_real(fuse_argc, fuse_argv, &peepfs_oper, sizeof(peepfs_oper), &PeepParams);

    return error;

}
