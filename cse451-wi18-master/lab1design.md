# Lab 1 Design Doc: Interrupts and System Calls

## Overview

The goal of this lab is to implement an interface for users to interact with persistent
media or with other I/O devices, without having to distinguish between them.

### Major Parts
File Interface: Provide an abstraction for the user that doesn't depend on the type of
"file". In user space, this will allow for seamless switching of "file" without large
changes in the code (ex: Reading input from a file vs reading from stdin, the method
for attaining bytes will be the same).

System Calls: The system call interface provides a barrier for the kernel to validate
user program input. This way, we can keep the I/O device state consistent. No user
program can directly affect the state of the kernel's structure. Furthermore, when we are
in the kernel code, we don't have to go through the syscall interface, which cuts down
superfluous error checking for trusted code.

## In-depth Analysis and Implementation

### File Interface

#### Bookkeeping
We need a structure to keep track of a logical file, things we need to keep track of:
- In memory reference count
- Whether the "file" is an on disk inode, or a pipe (later assignment)
- A reference to the inode or the file
- Current offset
- Access permissions (readable or writable) [for when we add pipes and file writeability later]

#### Kernel View
There will be a global array of all the open files on the system (bounded by `NFILE`) placed in
static memory.

#### Process View
Each process will have an array of open files (Bounded by `NOFILE`) in the process struct. We'll
use pointers to elements of the global open file table. The file descriptor will be the respective
index into the file table. (ex: stdin is typically file descriptor 0, so the corresponding file struct
will be the first element, a pointer into the global file table that is the stdin open file)

#### Functions

- `filewrite`, `fileread`:
  - Writing or reading of a "file", based on whether the file is an inode or a pipe.
- `fileopen`:
  - Finds an open file in the global file table to give to the process.
- `fileclose`:
  - Release the file from this process, will have to clean up if this is the last reference.
- `filedup`:
  - Will find an open spot in the process file table and have it point to the fd of the first
    duped file. Will need to update the reference count of the file.
- `filestat`:
  - Return statistics to the user about a file.

### System Calls

#### sys_open, sys_read, sys_write, sys_close, sys_dup, sys_fstat
Will need to parse arguments from the user and validate them (we never trust the user).
There are a few useful functions provided by xk:

All functions have `int n`, which will get the n'th argument. We need this because
we are direct reading the arguments from the registers. Returns 0 on success, -1 on failure

- `int argint(int n, int *ip)`: Gets an `int` argument
- `int argint64_t(int n, int64_t *ip)`: Gets a `int64_t` argument
- `int argptr(int n, char **pp, int size)`: Gets an array of `size`. Needs size
  to check array is within the bounds of the user's address space
- `int argstr(int n, char **pp)`: Tries to read a null terminated string.

Since all our system calls will be dealing with files, we think it will be useful to
add a function that validates a file descriptor:

- `int argfd(int n, int *fd)`: Will get the file descriptor, making sure it's a valid
  file descriptor (in the open file table for the process).

The main goals of the `sys_*` functions is to do argument parsing and then calling the
associated `file*` functions.

## Risk Analysis

### Unanswered Questions

- How will we handle pipes (another "file" type addressed in a later assignment)?
- What happens when two different process try to update data on the file?
- What happens when the user or the kernel has the maximum number of file's open?

### Staging of Work
First, the global file table will be implemented. Then the specific file functions. After, the user
open file table. Once the interface for files is complete, the system call portion will be scrubbing
the user input, validating it, and calling the respective file functions.

### Time Estimation

- File interface (__ hours)
  - Structures (__ hours)
  - Kernel portion (__ hours)
  - Process portion (__ hours)
- System calls (__ hours)
  - sys_open (__ hours)
  - sys_read (__ hours)
  - sys_write (__ hours)
  - sys_close (__ hours)
  - sys_dup (__ hours)
  - sys_fstat (__ hours)
- Edge cases and Error handling (__ hours)
