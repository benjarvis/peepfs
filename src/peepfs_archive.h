#ifndef __PEEPFS_ARCHIVE_H__
#define __PEEPFS_ARCHIVE_H__

#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define PEEPFS_FLAG_DIR         0x01
#define PEEPFS_FLAG_SEEKABLE    0x02

typedef struct peepfs_archive_entry {
    int64_t     index;
    int64_t     size;
    uint64_t    flags;
} peepfs_archive_entry_t;

typedef int (*peepfs_archive_enum_callback_t)(
    const char *filename, peepfs_archive_entry_t *entry, void *arg);

typedef void * (*peepfs_archive_open_t)(
    const char *filename);

typedef void   (*peepfs_archive_close_t)(
    void *archive);

typedef int    (*peepfs_archive_enumerate_t)(
    void *archive, peepfs_archive_enum_callback_t callback, void *arg);

typedef int (*peepfs_archive_entry_open_t)(
    void *archive, const char *name, peepfs_archive_entry_t *entry);

typedef void * (*peepfs_archive_file_open_t)(
    void *archive, peepfs_archive_entry_t *entry);

typedef void  (*peepfs_archive_file_close_t)(
    void *archive, void *file);

typedef ssize_t (*peepfs_archive_file_read_t)(
    void *archive, void *file, void *buffer, size_t offset, size_t len);

typedef struct peepfs_archive_ops {
    peepfs_archive_open_t           open;
    peepfs_archive_close_t          close;
    peepfs_archive_enumerate_t      enumerate;
    peepfs_archive_entry_open_t     entry_open;
    peepfs_archive_file_open_t      file_open;
    peepfs_archive_file_close_t     file_close;
    peepfs_archive_file_read_t      file_read;
} peepfs_archive_ops_t;

typedef struct peepfs_archive {
    peepfs_archive_ops_t       *ops;
    void                       *plugin_data;
} peepfs_archive_t;

typedef void peepfs_archive_file_t;

peepfs_archive_t * peepfs_archive_open(
    const char *path);

void peepfs_archive_close(
    peepfs_archive_t *archive);

int peepfs_archive_enumerate(
    peepfs_archive_t *archive, 
    peepfs_archive_enum_callback_t callback, void *arg);

int peepfs_archive_entry_open(
    peepfs_archive_t *archive, const char *name, peepfs_archive_entry_t *entry);

peepfs_archive_file_t * peepfs_archive_file_open(
    peepfs_archive_t *archive, peepfs_archive_entry_t *entry);

void peepfs_archive_file_close(
    peepfs_archive_t *archive, peepfs_archive_file_t *file);

int peepfs_archive_file_read(
    peepfs_archive_t *archive, peepfs_archive_file_t *file, 
    void *buffer, size_t offset, size_t len);

#endif
