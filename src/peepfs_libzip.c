#include "peepfs_archive.h"

#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <zip.h>
#include <pthread.h>

#define MIN(x,y) ((x) < (y) ? (x) : (y))

typedef struct libzip_archive {
    zip_t              *zip;
    pthread_mutex_t     lock;
} libzip_archive_t;

typedef struct libzip_file {
    zip_file_t *zipfile;
    int         error;
    int64_t     index;
    int64_t     offset;
    int         seekable;
} libzip_file_t;

void *
peepfs_libzip_open(const char *zipname)
{
    libzip_archive_t   *zip_archive;
    zip_t              *zip;
    int                 error, n;

    zip = zip_open(zipname, ZIP_RDONLY, &error);

    if (zip == NULL) {
        return NULL;
    }

    n = zip_get_num_entries(zip, 0);

    if (n <= 0) {
        zip_close(zip);
        return NULL;
    }

    zip_archive = (libzip_archive_t*)calloc(1,sizeof(libzip_archive_t));
    
    zip_archive->zip = zip;

    pthread_mutex_init(&zip_archive->lock, NULL);

    return zip_archive;
}

void
peepfs_libzip_close(void *plugin_data)
{
    libzip_archive_t *archive = (libzip_archive_t*)plugin_data;

    zip_close(archive->zip);

    free(archive);

}


int 
peepfs_libzip_enumerate(
    void *plugin_data,
    peepfs_archive_enum_callback_t enum_callback, 
    void *arg) 
{
     
    libzip_archive_t       *archive = (libzip_archive_t*)plugin_data;
    peepfs_archive_entry_t  entry;
    zip_stat_t              zstat;
    unsigned int            len, i, n, error = 0;

    pthread_mutex_lock(&archive->lock);

    n = zip_get_num_entries(archive->zip, 0);

    for (i = 0; i < n; ++i) {

        if (zip_stat_index(archive->zip, i, 0, &zstat)) {
            error = -1;
            goto out;
        }

        len = strlen(zstat.name);

        entry.flags = 0;

        if (zstat.name[len-1] == '/') {
            entry.flags |= PEEPFS_FLAG_DIR;
        }

        if (zstat.comp_method == 0) {
            entry.flags |= PEEPFS_FLAG_SEEKABLE;
        }

        entry.index = i;
        entry.size  = zstat.size;
    
        if (enum_callback(zstat.name, &entry, arg) < 0) {
            error = -1;
            goto out;
        }

    }

out:

    pthread_mutex_unlock(&archive->lock);

    return error;
}


int
peepfs_libzip_entry_open(
    void                   *plugin_data, 
    const char             *name,
    peepfs_archive_entry_t *entry)
{
    libzip_archive_t           *archive = (libzip_archive_t*)plugin_data;
    int                         error = 0;
    char                        scratch[4096];
    zip_stat_t                  zstat;

    pthread_mutex_lock(&archive->lock);

    error = zip_stat(archive->zip, name, 0, &zstat);

    entry->flags = 0;

    if (error) {
        entry->flags |= PEEPFS_FLAG_DIR;
        snprintf(scratch, sizeof(scratch), "%s/", name);
        error = zip_stat(archive->zip, scratch, 0, &zstat);
    }

    if (error) {
        error = -1;
        goto out;
    }

    entry->index    = zstat.index;
    entry->size     = zstat.size;

    if (zstat.comp_method == 0) {
        entry->flags |= PEEPFS_FLAG_SEEKABLE;
    }

out:

    pthread_mutex_unlock(&archive->lock);

    return error;
}

void *
peepfs_libzip_file_open(
    void *plugin_data,
    peepfs_archive_entry_t *entry)
{
    libzip_archive_t       *archive = (libzip_archive_t*)plugin_data;
    libzip_file_t          *file = NULL;
    zip_file_t             *zf;

    pthread_mutex_lock(&archive->lock);

    zf = zip_fopen_index(archive->zip, entry->index, 0);

    if (zf == NULL) {
        goto out;
    }

    file = (libzip_file_t*)calloc(1,sizeof(libzip_file_t));

    file->zipfile  = zf;
    file->index    = entry->index;
    file->offset   = 0;
    file->seekable = entry->flags & PEEPFS_FLAG_SEEKABLE;

out:

    pthread_mutex_unlock(&archive->lock);

    return file;
    
}

void 
peepfs_libzip_file_close(
    void *plugin_data,
    void *file_data)
{
    libzip_archive_t *archive = (libzip_archive_t*)plugin_data;
    libzip_file_t    *file  = (libzip_file_t*)file_data;

    pthread_mutex_lock(&archive->lock);

    zip_fclose(file->zipfile);

    pthread_mutex_unlock(&archive->lock);

    free(file);
}


ssize_t
peepfs_libzip_file_read(
    void                   *plugin_data,
    void                   *file_data,
    void                   *buffer,
    size_t                  offset,
    size_t                  size)
{
    libzip_archive_t   *archive = (libzip_archive_t*)plugin_data;
    libzip_file_t      *file  = (libzip_file_t*)file_data;
    int                 error;
    ssize_t             len = -1;

    pthread_mutex_lock(&archive->lock);

    if (file->error) {
        len = -1;
        goto out;
    }

    if (offset != file->offset) {

        /* If we think we can seek, try to do it */
        if (file->offset >= 0 && file->seekable) {
            error = zip_fseek(file->zipfile, offset, SEEK_SET);

            if (error == 0) {
                file->offset = offset;
            }
        }

        /* If we are past where we need to be, we need to start over */
        if (file->offset > offset) {

            zip_fclose(file->zipfile);

            file->zipfile = zip_fopen_index(archive->zip, file->index, 0);

            if (file->zipfile == NULL) {
                file->error = -1;
                len = -1;
                goto out;
            }

            file->offset  = 0;
        }

        /* Now read forward and toss data until we get to the desired
         * place
         */

        while (file->offset < offset) {

            len = MIN(size, offset - file->offset);

            len = zip_fread(file->zipfile, buffer, len);

            if (len <=  0) {
                file->error = 1;
                len = -1;
                goto out;
            }        

            file->offset += len;
        }
            
    }

    len = zip_fread(file->zipfile, buffer, size);

    if (len > 0) {
        file->offset += len;
    }

out:

    pthread_mutex_unlock(&archive->lock);

    return len; 

}

peepfs_archive_ops_t libzip_ops = {
    .open           = peepfs_libzip_open,
    .close          = peepfs_libzip_close,
    .enumerate      = peepfs_libzip_enumerate,
    .entry_open     = peepfs_libzip_entry_open,
    .file_open      = peepfs_libzip_file_open,
    .file_close     = peepfs_libzip_file_close,
    .file_read      = peepfs_libzip_file_read
};
