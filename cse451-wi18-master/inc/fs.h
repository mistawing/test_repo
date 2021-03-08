#pragma once

#include "extent.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define INODEFILEINO 0 // inode file inum
#define ROOTINO 1      // root i-number
#define BSIZE 512      // block size
#define EXTENT_N 7

// Disk layout:
// [ boot block | super block | free bit map |
//                                          inode file | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;       // Size of file system image (blocks)
  uint nblocks;    // Number of data blocks
  uint bmapstart;  // Block number of first free map block
  uint inodestart; // Block number of the start of inode file
  // Added in LAB 4
  uint swapstart;  // Block number of the start of swap region
  // Added in LAB 5
  uint logstart;   // Block number of the start of the log region

};

// On-disk inode structure
struct dinode {
  short type;         // File type
  short devid;        // Device number (T_DEV only)
  uint size;          // Size of file (bytes)
  struct extent data[EXTENT_N]; // Data blocks of file on disk
  //char pad[46];       // So disk inodes fit contiguosly in a block
};

// offset of inode in inodefile
#define INODEOFF(inum) ((inum) * sizeof(struct dinode))

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};


// Added in LAB 5
struct commit_block {
  uint dst_blocknos[40];
  uint commit_flag;       // indicates whether we are ready to commit or not
  uint size;
  char pad[BSIZE - sizeof(uint) * 42];
};


