#pragma once

#include <extent.h>
#include <sleeplock.h>
#include <param.h>

#define ON_DISK 0
#define ON_PIPE 1

#define SIZE 2048

#define EXTENT_N 7

// in-memory copy of an inode
struct inode {
  uint dev;  // Device number
  uint inum; // Inode number
  int ref;   // Reference count
  struct sleeplock lock;

  short type; // copy of disk inode
  short devid;
  uint size;
  struct extent data[EXTENT_N];
};

// in-memory copy of a pipe
struct pipe {
  int read_fd;
  int write_fd;
  int front;
  int tail;
  struct spinlock lock;
  char buf[SIZE];
};

// in-memory copy of file
struct file {
  uint64_t ref_count;
  int file_type;
  struct inode *inode;
  unsigned int offset;
  int permissions;
  int global_fd;
  struct pipe *pipe;
};

// finds an open spot in the process file table and points 
// it to the fd of the first duped file, updating the ref count
int filedup(struct proc *p, struct file *f);

// reads a file based on whether the file is an inode or a pipe
int fileread(struct file *f, char *buf, int bytes_read);

// writes to a file based on whether the file is an inode or a pipe
int filewrite(struct file *f, char *buf, int bytes_written);

// appeans to a file 
int fileappend(struct file *f, char *buf, int bytes_written);

// releases the file from the process, cleaning up if it is the 
// last reference
int fileclose(struct proc *p, struct file *f, int fd);

// returns the statistics to the user about a file
int filestat(struct file *f, struct stat *fstat);

// finds an open file in the global file table to give to the
// process
int fileopen(struct proc *p, struct inode *ptr, int mode);

// creates a pipe between two processes
int pipe(int *fds);

// table mapping device ID (devid) to device functions
struct devsw {
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

struct {
  struct spinlock lock;
  struct file file_table[NFILE];
  int valid_flags[NFILE];
} ftable;

// Device ids
enum {
  CONSOLE = 1,
};
