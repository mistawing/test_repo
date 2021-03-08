//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <proc.h>
#include <fcntl.h>

struct devsw devsw[NDEV];

int filestat(struct file *f, struct stat *fstat) {
  // use the stati method to populate the stat struct with the
  // information in the file's inode
  if (f->file_type == ON_DISK) {
    stati(f->inode, fstat);
    return 0;
  }
  return -1; 
}

int fileclose(struct proc *p, struct file *f, int fd) {
  // decrease the reference count of the file by 1 in the global file 
  // table. If its reference count is 1 before the decrement, then we 
  // just set its valid bit to 0 to indicatethat the file is closed,
  // and call irelease to free its inode on disk
  if (f->ref_count > 1) {
    ftable.file_table[f->global_fd].ref_count--; 
  } else {  // f->ref_count == 1
    ftable.valid_flags[f->global_fd] = 0;

    if (f->file_type == ON_DISK) {
      irelease(f->inode);
    } else {
      acquire(&f->pipe->lock);

      if (f->permissions == O_RDONLY) {
        wakeup(&ftable.file_table[f->global_fd].pipe->tail);
      } else if (f->permissions == O_WRONLY) {
        wakeup(&ftable.file_table[f->global_fd].pipe->front);
      }

      // if both the read and the write file descriptor for the pipe
      // are completely closed, free the page that is allocated for
      // the pipe
      if(ftable.valid_flags[f->pipe->read_fd] == 0 &&
         ftable.valid_flags[f->pipe->write_fd] == 0) {
        release(&f->pipe->lock);
        kfree((char*)f->pipe);
      } else {
        release(&f->pipe->lock);
      }
    }
  }

  // remove the file from the current process's file table
  p->file_table[fd] = NULL;
  return 0;
}

int fileread(struct file *f, char *buf, int bytes_read) {
  int res = -1;
  if (f->file_type == ON_DISK) {
    struct sleeplock *lock = &(f->inode->lock);
    acquiresleep(lock);

    res = readi(f->inode, buf, ftable.file_table[f->global_fd].offset, bytes_read);
    if (res >= 0) {
      ftable.file_table[f->global_fd].offset += res;
    }
    releasesleep(lock);
  } else if (f->file_type == ON_PIPE && f->global_fd == f->pipe->read_fd) {
    struct spinlock *lock = &(f->pipe->lock);
    acquire(lock);
    res = readp(f->pipe, buf, ftable.file_table[f->global_fd].offset, bytes_read);
    if (res >= 0) {
      ftable.file_table[f->global_fd].offset += res;
    }
    release(lock);
  }
  return res;
}

int filewrite(struct file *f, char *buf, int bytes_written) {
  int res = -1;
  if (f->file_type == ON_DISK) {
    struct sleeplock *lock = &(f->inode->lock);
    acquiresleep(lock);

    res = writei(f->inode, buf, f->offset, bytes_written);

    if (res >= 0) {
      f->offset += res;
      f->inode->size += res;
      updatei(f->inode);
    }

    releasesleep(lock);
  } else if (f-> file_type == ON_PIPE && f->global_fd == f->pipe->write_fd) {
    struct spinlock *lock = &(f->pipe->lock);
    acquire(lock);
    res = writep(f->pipe, buf, f->offset, bytes_written);
    if (res >= 0) {
      f->offset += res;
    }
    release(lock);
  }

  return res;
}

int filedup(struct proc *p, struct file *f) {
  for (int i = 0; i < NOFILE; i++) {
    if (p->file_table[i] == NULL) {
      p->file_table[i] = &(ftable.file_table[f->global_fd]);
      ftable.file_table[f->global_fd].ref_count++;
      return i;
    }
  }
  return -1;
}

int fileopen(struct proc *p, struct inode *ptr, int mode) {
  for (int i = 0; i < NFILE; i++) {
    if (ftable.valid_flags[i] == 0) {
      struct file newFile;
      newFile.ref_count = 1;
      newFile.file_type = ON_DISK;
      newFile.inode = ptr;
      newFile.offset = 0;
      newFile.permissions = mode;
      newFile.global_fd = i;

      ftable.file_table[i] = newFile;
      ftable.valid_flags[i] = 1;

      // Add to process file table
      for (int j = 0; j < NOFILE; j++) {
        if (p->file_table[j] == NULL) {
          p->file_table[j] = &(ftable.file_table[i]);
          return j;
        }
      }	
      
      // Reset if adding to proc file table fails
      ftable.valid_flags[i] = 0;
      return -1;
    }
  }
  
  return -1;
}

int checkProcFileTable(int *fds) {
  struct proc *p = myproc();
  int index = 0;
  for (int i = 0; i < NOFILE; i++) {
    if (p->file_table[i] == NULL) {
      fds[index] = i;
      index++;
    }
    if (index == 2)
      return 0;
  }
  return -1; 
}

int pipe(int *fds) {

  struct pipe *pipe_ptr = (struct pipe *) kalloc();
  if (pipe_ptr == NULL) {
    return -1;
  }

  int fd[2];
  int index = 0;

  // Check proc file table for two fds
  int proc_fd[2];
  if (checkProcFileTable(proc_fd) < 0) {
    kfree((char *) pipe_ptr);
    return -1;
  }

  // Check global file table for two fds
  for (int i = 0; i < NFILE; i++) {
    if (ftable.valid_flags[i] == 0 && index < 2) {
      ftable.valid_flags[i] = 1;
      fd[index] = i;
      index++;
    }
  }

  // Not enough space in the global file table.
  if (index <= 1) {
    if (index > 0) {
      ftable.valid_flags[fd[0]] = 0;
    }
    kfree((char *) pipe_ptr);
    return -1;
  }

  pipe_ptr->read_fd = fd[0];
  pipe_ptr->write_fd = fd[1];
  pipe_ptr->front = 0;
  pipe_ptr->tail = 0;
 
  // create readfile and writefile
  struct file read_file = {1, ON_PIPE, NULL, 0, O_RDONLY, fd[0], pipe_ptr};
  struct file write_file = {1, ON_PIPE, NULL, 0, O_WRONLY, fd[1], pipe_ptr};

  // add to global file table  
  ftable.file_table[fd[0]] = read_file;
  ftable.file_table[fd[1]] = write_file;

  // add to proc file table
  struct proc *p = myproc();
  p->file_table[proc_fd[0]] = &(ftable.file_table[fd[0]]);
  p->file_table[proc_fd[1]] = &(ftable.file_table[fd[1]]);

  // initialize the lock for this new pipe
  initlock(&(pipe_ptr->lock), "pipe");

  // return the fds used by the pipe through the parameter
  fds[0] = proc_fd[0];
  fds[1] = proc_fd[1];

  return 0;
}
