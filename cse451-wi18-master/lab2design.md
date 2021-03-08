# Lab 2 Design Doc: Multiprocessing

## Overview

The goals of this lab are to...
1. Maintain the functionality of the interface for communication that was 
    developed in lab 1 with concurrency 
2. Implement an interface for users to create, delay, and delete multiple 
    processes
3. Implement communication between processes via pipes
4. Implement UNIX exec which loads a new program onto an existing process

### Major Parts

Synchronization Issues: The code from lab 1 should be adapted so that it 
can successfully support concurrent systems calls. Different processes 
should be able to call the file interface functions without corrupting or 
modifying the global file table in an undesirable way which would result 
from the incorrect placement of a lock. Locks should cover all and only 
critical sections.

Fork: The syscall fork creates another process with the same state of the 
user-level application and returns twice - once in the parent with the return
value of the process ID (pid) and once in the child with the return value of
zero. The challenge of implementing fork would be duplicating the files and 
figuring out how to return twice. 

Wait/Exit: The syscall wait suspends execution of a process until one of 
its child processes finishes executing, returning the process ID of the child
process. On the other hand, the syscall exit halts the programs and returns 
resources such as kernel allocated pages back to the kernel heap so that they
maybe reused. For wait, the challenge would be checking whether the child 
process has terminated. For exit, the challenge would be to ensure all
resources that need to be reclaimed are made available again. 

Pipe: The pipe syscall creates a pipe (a holding area for written data) and
opens two file descriptors, one for reading and one for writing. The challenge
for this implementation is modifying the file struct such that it can also 
work with pipes and inodes.

Exec: The exec syscall loads a new program onto an existing process. The challenge 
of this syscall is the copying. Since we want an exact copy, we will need to go
through the virtual memory abstraction to the page table to access and copy the 
physical memory. 

## In-depth Analysis and Implementation 

### Synchronization Issues
- To address synchronization issues we need to place locks around critical sections
- the global file table will have a spin lock
- when any of the file functions are called, acquire and release the lock will be 
  called accordingly (because all functions access global file table)

### Functions

### Fork
In `kernel/proc.c`: `int fork(void)`
- Create a new entry in the process table via allocproc()
- Duplicate the user memory via vspacecopy()
- Duplicate the trap frame and all open files
- Setting the state of the child process to RUNNABLE
- Set the child process’s parent to the process we originally in
- Returns 0 for the child process, and the pid of the child for the parent process

### Wait/Exit
In `kernel/proc.c`: `int wait(void)`
- Suspend execution (make the process go to sleep) until a child has terminated 
  (i.e. the child has became a zombie)
- Reclaim the resources consumed by that child process via vspacefree()
- Return the pid of the terminated child, or -1 on error

In `kernel/proc.c`: `void exit(void)`
- Set the state of the process to ZOMBIE
- Close all the opened files within the process
- Wake up the parents to reclaim its resources
- Does not return anything

### Pipe
- We will need to define the pipe struct, which is just a 4kb kernel page that we 
  get from kalloc(), and it will will include:
  - a bounded buffer for storing the data
  - Two open file descriptors, which have their file permissions set accordingly - 
    the reading fd as RDONLY and the writing fd as WRONLY
- Set field variable for the two file structs accordingly to indicate that they are 
  pipes – will need to modify the structure of file struct to accommodate this
- Use a spinlock to lock the bounded buffer whenever we are writing to it or reading 
  from it
- We also need to implement functions for reading from and writing into pipes
- Return 0 on success, or -1 on error

### Exec
In `kernel/exec.c`: `int exec(int argc, char**argv)`
- Call vspaceloadcode() to read the program and load it into the passed in address space
- Create a deep copy of the argument from one user stack to another user stack 
  through vspacewritetova()
- Install the virtual address space when the process is ready to run with 
  vspaceinstall(myproc())
- Don’t return anything, or -1 if there is an error


### System Calls

We will use the following functions to parse the argument from the user:
- `int argint(int n, int *ip)` -> parse an int argument
- `int argint64_t(int n, int64_t *ip)` -> parse a int64_t argument
- `int argstr(int n, char **pp)` -> parse a null terminated string

kernel/sysproc.c: sys_fork
- Error cases to check and potential fixes:
  - Kernel lacks space to create new process -> check by examining the 
    return value of allocproc()

kernel/sysproc.c: sys_wait
- Error cases to check and potential fixes:
  - The calling process did not create any child process -> check by going 
    through the ptable and count how many processes have our current process 
    as the parent
  - All child processes have been returned in previous calls to wait

kernel/sysproc.c: sys_exit
- Error cases to check and potential fixes: None

kernel/sysfile.c: sys_pipe
- Error cases to check and potential fixes:
  - arg0 contains invalid address -> check through method 
    `argptr(int n, char **pp, int size)`
  - Kernel does not have space to create pipe -> check if kalloc() returns 0
  - Kernel does not have two available file descriptors -> check by going 
    through the global file table and count how many file descriptors are available

kernel/sysfile.c: sys_exec
- Error cases to check and potential fixes
  - arg0 points to an invalid or unmapped address -> check through method 
    `argptr(int n, char **pp, int size)`
  - there is an invalid address before the end of the arg0 string -> check 
    using method argstr mentioned above
  - arg0 is not a valid executable file, or it cannot be opened -> check by 
    calling open() and stat() on the file
  - the kernel lacks space to execute the program
  - arg1 points to an invalid or unmapped address -> check by using method argptr
  - there is an invalid address between arg1 and the first n st arg1[n] == `\0` -> 
    check through method `argptr(int n, char **pp, int size)`
  - for any i < n, there is an invalid address between arg1[0] and the first `\0` -> 
    check through method `argptr(int n, char **pp, int size)`
  - Check number of arguments is less than or equal to MAXARG -> go through the 
    argument array and check how many arguments there are


## Risk Analysis

### Unanswered Questions
kernel/proc.c:fork
- how do we actually return twice?

kernel/proc.c:wait
- how to return the pid of the terminated child process to its parent process?

kernel/sysfile.c:sys_exec
- how do we check if the kernel lacks space to execute the program?

kernel/sysproc.c:sys_wait
- how do we know whether all child processes of a process have been returned 
  in the previous calls to wait or not?

### Staging of Work (Time Estimation)
- Synchronization issues: 1-2 hours
 - sys_fork: 1-2 hours
 - sys_exit: 0.5-1 hour
 - sys_wait: 4-5 hours
 - sys_pipe: 3-4 hours
 - sys_exec: 3-4 hours
