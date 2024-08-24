# peepfs

## Purpose

PeepFS is a Linux FUSE file system that is layered on top of an ordinary base
file system and provides transparent inspection and extraction of archive files
(.zip, .tar, etc).

Any place where a supported archive file is found in the base file system, a
directory tree representing the contents of that archive file is presented in
the peepfs file system as though the archive had been extracted.  File operations
on these synthetic, extracted contents are transparently translated into
operations against the contents of the base archive file.  File operations on
non-archive files are passed through to the base file system directly as-is.

All mapping of archive contents to the peepfs namespace are done on the fly,
with no extra copies of archive data, and support random access.

A configurable cache is provided to avoid repeated interrogation of the archive
files for metadata.   The kernel buffer cache is relied upon for data caching.

## Status

This project is basically abandoned.  I threw it together for a prototype many
years ago that never went anywhere.  It was originally written for FUSE 2 but
I've quickly updated it to run with FUSE 3 on Ubuntu 24 just to confirm it still
mostly works.  If you actually plan on using this for anything serious you will
need to do some work.

## Building

```
$ sudo apt install cmake build-essential libzip-dev libarchive-dev libfuse3-dev
$ mkdir build
$ cd build
$ cmake ../src
-- The C compiler identification is GNU 13.2.0
-- The CXX compiler identification is GNU 13.2.0
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /usr/bin/cc - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/c++ - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Configuring done (0.6s)
-- Generating done (0.0s)
-- Build files have been written to: /sandboxen/peepfs/build
$ make
[ 20%] Building C object CMakeFiles/peepfs.dir/peepfs.c.o
[ 40%] Building C object CMakeFiles/peepfs.dir/peepfs_archive.c.o
[ 60%] Building C object CMakeFiles/peepfs.dir/peepfs_libzip.c.o
[ 80%] Building C object CMakeFiles/peepfs.dir/peepfs_libarchive.c.o
[100%] Linking C executable peepfs
[100%] Built target peepfs
```

## Usage
Where /test is my base file system and /mnt is where I want the peepfs fuse mount:
```
# ./peepfs -f /mnt /test 
```

Then we can explore the mount:

```
# find /test
/test
/test/mystuff.zip
/test/mystuff.tar
/test/regular_file.txt
root@easygoing-swordfish:~# unzip -v /test/mystuff.zip 
Archive:  /test/mystuff.zip
 Length   Method    Size  Cmpr    Date    Time   CRC-32   Name
--------  ------  ------- ---- ---------- ----- --------  ----
       0  Stored        0   0% 2024-08-24 23:14 00000000  contents/
      24  Stored       24   0% 2024-08-24 23:14 9326db0b  contents/inside_file.txt
--------          -------  ---                            -------
      24               24   0%                            2 files
root@easygoing-swordfish:~# tar tvf /test/mystuff.tar 
drwxrwxr-x root/root         0 2024-08-24 23:18 ./contents/
-rw-rw-r-- root/root        24 2024-08-24 23:14 ./contents/inside_file.txt
root@easygoing-swordfish:~# find /mnt
/mnt
/mnt/mystuff.zip
/mnt/mystuff.zip.peep
/mnt/mystuff.zip.peep/contents
/mnt/mystuff.zip.peep/contents/inside_file.txt
/mnt/mystuff.tar
/mnt/mystuff.tar.peep
/mnt/mystuff.tar.peep/contents
/mnt/mystuff.tar.peep/contents/inside_file.txt
/mnt/regular_file.txt
root@easygoing-swordfish:~# cat /mnt/regular_file.txt 
I am a regular file
root@easygoing-swordfish:~# cat /mnt/mystuff.tar.peep/contents/inside_file.txt 
I am inside a zip file
```

Note that a directory with the extension .peep is listed for each archive,
and then we can access the conents of the archive as though they were normal
files.
