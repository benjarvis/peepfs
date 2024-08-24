#include <stdlib.h>

#include "peepfs_archive.h"

extern peepfs_archive_ops_t libzip_ops;
extern peepfs_archive_ops_t libarchive_ops;

peepfs_archive_t *
peepfs_archive_open(const char *path)
{
    peepfs_archive_t *archive;
    void             *plugin_data;
    const char       *dot;

    dot = rindex(path, '.');

    if (strcasecmp(dot,".zip") == 0) {

        plugin_data = libzip_ops.open(path);

        if (plugin_data) {

            archive = (peepfs_archive_t*)calloc(1,sizeof(peepfs_archive_t));

            archive->ops = &libzip_ops;
            archive->plugin_data = plugin_data;

            return archive;
        }

    } else {

        plugin_data = libarchive_ops.open(path);

        if (plugin_data) {

            archive = (peepfs_archive_t*)calloc(1,sizeof(peepfs_archive_t));

            archive->ops = &libarchive_ops;
            archive->plugin_data = plugin_data;

            return archive;
        }

    }

    return NULL;
}

void 
peepfs_archive_close(peepfs_archive_t *archive)
{
    archive->ops->close(archive->plugin_data);
    free(archive);
}

int
peepfs_archive_enumerate(
    peepfs_archive_t *archive,
    peepfs_archive_enum_callback_t callback, 
    void *arg)
{
    return archive->ops->enumerate(archive->plugin_data, callback, arg);
}

peepfs_archive_file_t *
peepfs_archive_file_open(
    peepfs_archive_t *archive,
    peepfs_archive_entry_t   *entry)
{
    return archive->ops->file_open(archive->plugin_data, entry);
}

void
peepfs_archive_file_close(
    peepfs_archive_t *archive,
    peepfs_archive_file_t    *file)
{
    archive->ops->file_close(archive->plugin_data, file);
}

int
peepfs_archive_entry_open(
    peepfs_archive_t *archive,
    const char *name,
    peepfs_archive_entry_t *entry)
{
    return archive->ops->entry_open(archive->plugin_data, name, entry);
}

int
peepfs_archive_file_read(
    peepfs_archive_t       *archive,
    peepfs_archive_file_t  *file,
    void                   *buffer, 
    size_t                  offset, 
    size_t                  len)
{
    return archive->ops->file_read(archive->plugin_data, file, buffer, offset, len);
}


