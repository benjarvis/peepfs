#include "peepfs_archive.h"

#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <archive.h>
#include <archive_entry.h>
#include <pthread.h>

#define MIN(x,y) ((x) < (y) ? (x) : (y))

typedef struct libarchive_archive {
    const char         *filename;
} libarchive_archive_t;

typedef struct libarchive_file {
    struct archive         *arc;
    struct archive_entry   *entry;
    int64_t                 index;
    int64_t                 offset;
    int                     error;
    pthread_mutex_t         lock;
} libarchive_file_t;

static inline struct archive *
__peepfs_libarchive_open(const char *filename, int64_t seek_index)
{
    struct archive         *arc;
    struct archive_entry   *ae;
    int64_t                 i, error;

    arc = archive_read_new();

    archive_read_support_filter_all(arc);
    archive_read_support_format_all(arc);

    error = archive_read_open_filename(arc, filename, 10240);

    if (error != ARCHIVE_OK) {
        archive_read_free(arc);
        return NULL;
    }

    if (seek_index >= 0) {

        i = 0;

        while (archive_read_next_header(arc, &ae) == ARCHIVE_OK) {

            if (i == seek_index) {
                break;
            }

            archive_read_data_skip(arc);

            i++;
        }

        if (i != seek_index) {
            archive_read_free(arc);
            return NULL;
        }
    }

    return arc;
}

void *
peepfs_libarchive_open(const char *zipname)
{
    libarchive_archive_t   *archive;
    struct archive         *arc;

    arc = __peepfs_libarchive_open(zipname, -1);

    if (arc) {

        archive_read_free(arc);

        archive = (libarchive_archive_t*)calloc(1,sizeof(libarchive_archive_t));
   
        archive->filename = strdup(zipname); 

        return archive;

    } else {

        return NULL;

    }

}

void
peepfs_libarchive_close(void *plugin_data)
{
    libarchive_archive_t *archive = (libarchive_archive_t*)plugin_data;

    free((void*)archive->filename);
    free(archive);

}


int 
peepfs_libarchive_enumerate(
    void *plugin_data,
    peepfs_archive_enum_callback_t enum_callback,
    void *arg) 
{
    libarchive_archive_t   *archive = (libarchive_archive_t*)plugin_data;
    peepfs_archive_entry_t  entry;
    struct archive         *arc = NULL;
    struct archive_entry   *ae;
    unsigned int            i, error = 0;
    const char             *name;

    arc = __peepfs_libarchive_open(archive->filename, -1);

    if (arc == NULL) {
        error = -1;
        goto out;
    }

    i = 0;

    while (archive_read_next_header(arc, &ae) == ARCHIVE_OK) {
          
        name = archive_entry_pathname(ae);

        if (name[0] == '.' && name[1] == '/') {
            name+=2;
        }

        entry.flags = 0;
        entry.index = i;

        if (S_ISDIR(archive_entry_filetype(ae))) {
            entry.flags |= PEEPFS_FLAG_DIR;
        }

        entry.size = archive_entry_size(ae);
 
        if (enum_callback(name, &entry, arg) < 0) {
            error = -1;
            goto out;
        }

        archive_read_data_skip(arc);

        i++; 
    }

out:

    if (arc) archive_read_free(arc);

    return error;
}


int
peepfs_libarchive_entry_open(
    void                   *plugin_data, 
    const char             *name,
    peepfs_archive_entry_t *entry)
{
    libarchive_archive_t       *archive = (libarchive_archive_t*)plugin_data;
    int                         i, found = 0, name_len, arc_name_len;
    int                         error = 0;
    const char                 *arc_name;
    struct archive             *arc = NULL;
    struct archive_entry       *ae;

    arc = __peepfs_libarchive_open(archive->filename, -1);

    if (arc == NULL) {
        error = -1;
        goto out;
    }

    i = 0;

    name_len = strlen(name);

    while (archive_read_next_header(arc, &ae) == ARCHIVE_OK) {

        arc_name = archive_entry_pathname(ae);

        if (arc_name[0] == '.' && arc_name[1] == '/') {
            arc_name+=2;
        }

        arc_name_len = strlen(arc_name);

        while (arc_name[arc_name_len-1] == '/') {
            arc_name_len--;
        }

        if (arc_name_len == name_len) {

            if (strncmp(name, arc_name, arc_name_len) == 0) {
                found = 1;
                break;
            }
        }

        archive_read_data_skip(arc);
        i++;
    }

    if (!found) {
        error = -1;
        goto out;
    }

    entry->flags = 0;
    entry->index = i;

    if (S_ISDIR(archive_entry_filetype(ae))) {
        entry->flags |= PEEPFS_FLAG_DIR;
    }

    entry->size = archive_entry_size(ae);

out:

    if (arc) archive_read_free(arc);

    return error;
}

void *
peepfs_libarchive_file_open(
    void *plugin_data,
    peepfs_archive_entry_t *entry)
{
    libarchive_archive_t   *archive = (libarchive_archive_t*)plugin_data;
    libarchive_file_t      *file = NULL;
    struct archive         *arc;

    arc = __peepfs_libarchive_open(archive->filename, entry->index);

    if (arc == NULL) {
        goto out;
    }

    file = (libarchive_file_t*)calloc(1,sizeof(libarchive_file_t));

    file->arc      = arc;
    file->index    = entry->index;
    file->offset   = 0;

    pthread_mutex_init(&file->lock, NULL);

    arc = NULL;

out:

    if (arc) archive_read_free(arc);

    return file;
    
}

void 
peepfs_libarchive_file_close(
    void *plugin_data,
    void *file_data)
{
    libarchive_file_t    *file  = (libarchive_file_t*)file_data;

    archive_read_free(file->arc);
    free(file);
}


ssize_t
peepfs_libarchive_file_read(
    void                   *plugin_data,
    void                   *file_data,
    void                   *buffer,
    size_t                  offset,
    size_t                  size)
{
    libarchive_archive_t   *archive = (libarchive_archive_t*)plugin_data;
    libarchive_file_t      *file  = (libarchive_file_t*)file_data;
    ssize_t                 len;

    pthread_mutex_lock(&file->lock);

    if (file->error) {
        len = -1;
        goto out;
    }

    if (offset != file->offset) {

        /* If we are past where we need to be, we need to start over */
        if (file->offset > offset) {

            archive_read_free(file->arc);
            file->arc = __peepfs_libarchive_open(archive->filename, file->index);

            if (file->arc == NULL) {
                file->error = -1;
                len = - 1;
                goto out;
            }

            file->offset  = 0;

        }

        /* Now read forward and toss data until we get to the desired
         * place
         */

        while (file->offset < offset) {

            len = MIN(size, offset - file->offset);

            len = archive_read_data(file->arc, buffer, len);

            if (len <=  0) {
                file->error = -1;
                len = -1;
                goto out;
            }        

            file->offset += len;
        }
            
    }

    len = archive_read_data(file->arc, buffer, size);

    if (len > 0) {
        file->offset += len;
    }

out:

    pthread_mutex_unlock(&file->lock);

    return len; 

}

peepfs_archive_ops_t libarchive_ops = {
    .open           = peepfs_libarchive_open,
    .close          = peepfs_libarchive_close,
    .enumerate      = peepfs_libarchive_enumerate,
    .entry_open     = peepfs_libarchive_entry_open,
    .file_open      = peepfs_libarchive_file_open,
    .file_close     = peepfs_libarchive_file_close,
    .file_read      = peepfs_libarchive_file_read
};
