#ifndef STRATUM_KERNEL_VFS_FORMAT_H
#define STRATUM_KERNEL_VFS_FORMAT_H

#define VFS_MAX_NAME        64

/* File Mode Bits */
#define VFS_MODE_IFMT       0xF000
#define VFS_MODE_IFREG      0x8000
#define VFS_MODE_IFDIR      0x4000
#define VFS_MODE_IFCHR      0x2000
#define VFS_MODE_IFBLK      0x6000
#define VFS_MODE_IFIFO      0x1000

#endif
