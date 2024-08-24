cmake_minimum_required(VERSION 3.28)

project(peepfs)

set(PEEPFS_VERSION 1.0)

add_definitions(-g -O3 -Wall -D_FILE_OFFSET_BITS=64)
add_definitions(-Wno-strict-aliasing -Wno-format-truncation)
add_definitions(-DPEEPFS_VERSION_STR="${PEEPFS_VERSION}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(peepfs peepfs.c peepfs_archive.c peepfs_libzip.c peepfs_libarchive.c)

target_link_libraries(peepfs pthread fuse3 zip archive)

install(TARGETS peepfs DESTINATION ${DESTDIR}/sbin)
