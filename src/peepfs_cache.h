#ifndef __PEEPFS_CACHE_H__
#define __PEEPFS_CACHE_H__

#include "peepfs_archive.h"
#include "utlist.h"
#include "uthash.h"

typedef struct peepfs_cache_entry {
    uint64_t                    id;
    uint64_t                    archive_id;
    int64_t                     expire;
    const char                 *archivepath;
    const char                 *path;
    const char                 *relpath;
    peepfs_archive_entry_t      entry;
    struct peepfs_cache_entry  *prev;
    struct peepfs_cache_entry  *next;
    struct peepfs_cache_entry  *prev_by_expire;
    struct peepfs_cache_entry  *next_by_expire;
    struct peepfs_cache_entry  *next_by_dir;
    UT_hash_handle              hh;
} peepfs_cache_entry_t;

typedef struct peepfs_cache {
    peepfs_cache_entry_t   *hash;
    peepfs_cache_entry_t   *lru;
    peepfs_cache_entry_t   *expire;
    uint64_t                next_id;
    int64_t                 num_entries;
    int64_t                 max_entries;
    int64_t                 grace;
    pthread_mutex_t         lock;
} peepfs_cache_t;

static inline peepfs_cache_t *
peepfs_cache_init(int64_t max_entries, int64_t grace)
{
    peepfs_cache_t *cache;

    cache = (peepfs_cache_t*)calloc(1,sizeof(peepfs_cache_t));

    cache->max_entries  = max_entries;
    cache->grace        = grace;
    cache->next_id      = 1;

    pthread_mutex_init(&cache->lock, NULL);

    return cache;
}

void
peepfs_cache_free(peepfs_cache_t *cache)
{
    peepfs_cache_entry_t *e;

    while (cache->lru) {
        e = cache->lru;
        DL_DELETE(cache->lru, e);
        HASH_DEL(cache->hash, e);
        free((void*)e->archivepath);
        free((void*)e->path);
        free(e);

    }

    free(cache);
}

static inline void
__peepfs_cache_delete(
    peepfs_cache_t         *cache,
    peepfs_cache_entry_t   *e)
{
    peepfs_cache_entry_t *ae;

    if (e->archive_id) {
        HASH_FIND_STR(cache->hash, e->archivepath, ae);

        if (ae && ae->id == e->archive_id) {
            __peepfs_cache_delete(cache, ae);
        }
    }

    DL_DELETE(cache->lru, e);
    DL_DELETE2(cache->expire, e, prev_by_expire, next_by_expire);
    HASH_DEL(cache->hash, e);
    if (e->archivepath) free((void*)e->archivepath);
    if (e->relpath)     free((void*)e->relpath);
    if (e->path)        free((void*)e->path);

    free(e);

    --cache->num_entries;
}

static inline void
__peepfs_cache_expunge(peepfs_cache_t *cache)
{
    peepfs_cache_entry_t *e;
    int64_t now = time(NULL);

    while (1) {

        e = cache->expire;

        if (e && e->expire < now) {
            __peepfs_cache_delete(cache, e);
        } else {
            return;
        }

    }
    
}


static inline uint64_t
peepfs_cache_insert(
    peepfs_cache_t         *cache, 
    const char             *archivepath,
    const char             *relpath, 
    uint64_t                archive_id,
    peepfs_archive_entry_t *entry)
{
    peepfs_cache_entry_t *e, *ae = NULL;
    uint64_t id;
    time_t now = time(NULL);
    char fullpath[PATH_MAX];

    if (relpath) {
        snprintf(fullpath, PATH_MAX, "%s/%s", archivepath, relpath);
    } else {
        snprintf(fullpath, PATH_MAX, "%s", archivepath);
    }

    pthread_mutex_lock(&cache->lock);

    __peepfs_cache_expunge(cache);

    id = cache->next_id++;

    HASH_FIND_STR(cache->hash, fullpath, e);

    if (e) {
        __peepfs_cache_delete(cache, e);
    }

    if (cache->num_entries == cache->max_entries) {
        e = cache->lru;
        __peepfs_cache_delete(cache, e);
    }

    e = (peepfs_cache_entry_t*)calloc(1,sizeof(peepfs_cache_entry_t));

    if (e == NULL) {
        pthread_mutex_unlock(&cache->lock);
        return 0;
    }
   
    if (archivepath) e->archivepath = strdup(archivepath);
    if (relpath) e->relpath = strdup(relpath);

    e->path = strdup(fullpath);

    HASH_ADD_STR(cache->hash, path, e);

    ++cache->num_entries;

    if (entry) {
        e->entry = *entry;
    }

    e->id = id;
    e->expire = now + cache->grace;

    DL_APPEND(cache->lru, e);
    DL_APPEND2(cache->expire, e, prev_by_expire, next_by_expire);

    if (archive_id) {

        e->archive_id = archive_id;

        HASH_FIND_STR(cache->hash, archivepath, ae);

        if (ae && ae->id == archive_id) {
            DL_DELETE(cache->lru, ae);
            DL_APPEND(cache->lru, ae);
            LL_APPEND2(ae, e, next_by_dir);
        }
    }

    pthread_mutex_unlock(&cache->lock);

    return id;
}

static inline int
peepfs_cache_get(
    peepfs_cache_t         *cache,
    const char             *archivepath,
    const char             *relpath,
    peepfs_archive_entry_t *entry)
{
    peepfs_cache_entry_t   *e;
    int                     error = -1;
    char                    fullpath[PATH_MAX];

    snprintf(fullpath, PATH_MAX, "%s/%s", archivepath, relpath);

    pthread_mutex_lock(&cache->lock);
    
    __peepfs_cache_expunge(cache);

    HASH_FIND_STR(cache->hash, fullpath, e);

    if (e) {
        *entry = e->entry;
        DL_DELETE(cache->lru, e);
        DL_APPEND(cache->lru, e);
        error = 0;
    }

    pthread_mutex_unlock(&cache->lock);

    return error;
}

static inline int
peepfs_cache_scandir(
    peepfs_cache_t                 *cache,
    const char                     *archivepath,
    peepfs_archive_enum_callback_t  enum_callback,
    void                           *arg)
{
    peepfs_cache_entry_t *ae,*e;
    int                   error = -1;

    pthread_mutex_lock(&cache->lock);

    __peepfs_cache_expunge(cache);

    HASH_FIND_STR(cache->hash, archivepath, ae);

    if (ae) {

        LL_FOREACH2(ae->next_by_dir, e, next_by_dir) {

            DL_DELETE(cache->lru, e);
            DL_APPEND(cache->lru, e);
            error = enum_callback(e->relpath, &e->entry, arg);

            if (error) {
                break;
            }
        }

        DL_DELETE(cache->lru, ae);
        DL_APPEND(cache->lru, ae);
    }

    pthread_mutex_unlock(&cache->lock);

    if (ae) {
        return 0;
    } else {
        return -1;
    }
}
    
#endif
