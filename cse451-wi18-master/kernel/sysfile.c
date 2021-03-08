//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>


void finit(void) {
  initlock(&ftable.lock, "ftable");
}

int sys_dup(void) {
  int fd;		// arg0: file descriptor

  // Error Conditions:
  // B1: fd is not an open file descriptor
  if (argfd(0, &fd) < 0)
    return -1;

  // check whether the valid bit for the given fd is set to
  // 1 in the global file table
  struct proc *p = myproc();
  struct file f = *(p->file_table[fd]);

  acquire(&ftable.lock);
  if (ftable.valid_flags[f.global_fd] == 0) {
    release(&ftable.lock);
    return -1;
  }

  int res = filedup(p, &f);

  release(&ftable.lock);
  return res;
}

int sys_read(void) {
  int fd; 		// arg0: file descriptor
  char *buf;		// arg1: buffer to write bytes to
  int bytes_read;	// arg2: number of bytes to read

  // Error Conditions:
  // B1: fd is a not a file descriptor open for read
  if (argfd(0, &fd) < 0)
    return -1;

  // check whether the valid bit for the given fd is set to 1 in
  // the global file table, and that its access permissions includes
  // read permission
  struct file f = *(myproc()->file_table[fd]);
  
  acquire(&ftable.lock);
  if (ftable.valid_flags[f.global_fd] == 0 || f.permissions == O_WRONLY) {
    release(&ftable.lock);
    return -1;
  }

  release(&ftable.lock);

  // B3: number of bytes to read is not positive.
  argint(2, &bytes_read);

  if (bytes_read < 0)
    return -1;

  // B2: some addres between [arg1, arg1+arg2-1] is invalid
  if (argptr(1, &buf, bytes_read) < 0)
    return -1;

  int res = fileread(&f, buf, bytes_read);
  return res;
}

int sys_write(void) {
  int fd;		// arg0: file descriptor
  char *buf;		// arg1: buffer of bytes to write to fd
  int bytes_written;	// arg2: number of bytes to write

  // Error Conditions:
  // B1: fd is not a file descriptor open for write
  if (argfd(0, &fd) < 0)
    return -1;

  // check whether the valid bit for the given fd is set to 1 in
  // the global file table, and that its access permissions includes
  // write permission
  //struct file f = *(myproc()->file_table[fd]);

  struct file *f = myproc()->file_table[fd];

  acquire(&ftable.lock);
  if (ftable.valid_flags[f->global_fd] == 0 || f->permissions == O_RDONLY) {
    release(&ftable.lock);
    return -1;
  }

  release(&ftable.lock);

  // B3: number of bytes to write not positive
  argint(2, &bytes_written);
  if (bytes_written < 0)
    return -1;
  
  // B2: some address between [arg1, arg1+arg2-1] is invalid
  if (argptr(1, &buf, bytes_written) < 0 || argstr(1, &buf) < 0)
    return -1;
  
  int res;

  res = filewrite(f, buf, bytes_written);
  return res;
}

int sys_close(void) {
  int fd;		// arg0: file descriptor

  // Error Conditions:
  // B1: fd is not an open file descriptor
  if (argfd(0, &fd) < 0)
    return -1;

  // chcek whether the valid bit for the given fd is set to 1
  // in the global file table
  struct proc *p = myproc();
  struct file f = *(p->file_table[fd]);

  acquire(&ftable.lock);
  if (ftable.valid_flags[f.global_fd] == 0) {
    release(&ftable.lock);
    return -1;
  }

  int res = fileclose(p, &f, fd);

  release(&ftable.lock);
  return res;
}

int sys_fstat(void) {
  int fd;		// arg0: file descriptor
  struct stat *fstat; 	// arg1: stat struct to populate

  // Error Conditions:
  // B1: fd is not an open file descriptor
  if (argfd(0, &fd) < 0)
    return -1;

  // check whether the valid bit for the given fd is set to 1 in
  // the global file table 
  struct file f = *(myproc()->file_table[fd]);

  acquire(&ftable.lock);
  if (ftable.valid_flags[f.global_fd] == 0) {
    release(&ftable.lock);
    return -1;
  }

  release(&ftable.lock);

  // B2: there is an invalid address between [arg1, arg1+sizeof(stat)]
  if (argptr(1, (char**)(&fstat), sizeof(fstat)) == -1)
    return -1;

  acquiresleep(&(f.inode->lock));
  int res = filestat(&f, fstat);
  releasesleep(&(f.inode->lock));
  return res;
}

int sys_open(void) {
  char *filepath;	// arg0: path to file
  int mode;		// arg1: mode for opening file

  // Error Conditions:
  // B1: filepath points to an invalid or unmapped address
  // B2: there is an invalid address before end of string
  argint(1, &mode);
  if (argstr(0, &filepath) < 0 || argptr(0, &filepath, strlen(filepath)) < 0)
    return -1;

  // B3 + B4: check if file DNE and mode != O_CREATE
  struct inode *ptr = namei(filepath);

  if ((mode & 0xF00) != O_CREATE && ptr == NULL) 
    return -1;

  // Check if a valid permision has been passed
  if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR && mode != O_CREATE && 
      (mode != (O_CREATE | O_RDWR))) {
    return -1;
  }

  if ((mode & 0xF00) == O_CREATE && ptr == NULL) {
    ptr = icreate(filepath);
  }

  acquire(&ftable.lock);
  int res = fileopen(myproc(), ptr, mode);
  release(&ftable.lock);
  return res;
}

int sys_exec(void) {
  char * filepath; 		// arg0: path to the exe file
  char * arguments[MAXARG]; 	// arg1: array of strings for arguments

  // check if arg0 points to an invalid or unmapped address
  // or if there is an invalid address before the end of the string
  if (argstr(0, &filepath) < 0 || argptr(0, &filepath, strlen(filepath)) < 0)
    return -1;

  int addr;
  // check if the arg1 points to an invalid or unmapped address
  if (argint(1, &addr) < 0) {
    return -1;
  }

  // loop through the arguments until a null argument is reached
  // and then call execute.
  for (int i = 0; i < MAXARG; i++) { 

    // fetch address of the ith argument
    int arg_addr;
    if (fetchint(addr + i * 8, &arg_addr) < 0) {
      return -1;
    }

    // fetch the ith argument using its address
    if (fetchstr(arg_addr, &arguments[i]) < 0) {
      return -1;
    }

    if (arguments[i] == '\0') {
      return exec(i, filepath, arguments);
    }
  }

  // number of arguments exceed maximum number of arguments
  return -1;
}

int sys_pipe(void) {
  int * fds; 		// arg0: pointer to an array of two fds

  // check if arg0 points to an invalid or unmapped address
  if (argptr(0, (char **) &fds, sizeof(int) * 2) < 0)
    return -1;

  // create pipe
  acquire(&ftable.lock);
  int res = pipe(fds);
  release(&ftable.lock);

  return res;
}
