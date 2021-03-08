## Lab 3 Design Doc: Address Space Management

Group member: Hae In Lee, Lemei Zhang

## Overview

The goal of this lab is to create a conservative, address space management  system 
for xk and to be able to run xk’s shell in addition to running several commands such 
as `grep`, `cat`, and `echo`. 


### Major Parts

Create a user-level heap: To support heaps, the call sbrk (set program to break) 
should be implemented. When a process runs out of memory or asks to allocate a data 
region that cannot fit on the current heap, sbrk should be called, increasing the 
size of the heap if possible and keeping track of the current size of the heap. 
Unlike kmalloc and kfree which allocates and frees memory per page, sbrk should 
support memory allocation and deallocation at the byte-level. 

Run a shell and commands: Since the shell is already implemented for us, our task is 
to load it after the xk boots. Assuming our exec call works correctly, we should be 
able to run commands such as `cat`, `echo`, and `ls`.

Increment user stack on-demand: We will have to change the behavior of the user stack 
such that it allocates only the memory that is needed at run-time and grows only 
on-demand. At the start of a user-process, we will begin with one page to store 
application arguments. To implement an on-demand growing stack, we will have to handle 
page faults (a hardware exception that will trap into the kernel) by adding memory to 
the stack region and then resuming execution.

Copy-on-write fork: We will implement an optimization to our current fork function such 
that related processes will share a physical address space until one of the process’s 
need to perform a write. Only on a write will a copy of the page, where memory was written 
to, be made. To implement this, we will have set all pages to read only and wait for a 
trap that will bring us into the kernel. In the kernel, we will allocate a new page.


## In-depth Analysis and Implementation

### Functions

### sbrk
`char *sbrk(int n)`
- If the current “unused” space inside the user heap can already support the given number 
  of bytes (unused space = vspace.regions[VR_USTACK].va_base - vspace.regions[VR_USTACK].size - 
  (vspace.regions[VR_HEAP].va_base + vspace.regions[VR_HEAP].size)), skip the next step
- Else, if “unused” lacks space, call vregionaddmap()  on the heap’s vregion with sz = n 
  (which adds a page to this region). If vregionaddmap() fails, return -1 and exit.
- vspace.regions[VR_HEAP].size += n;

- call for vregionaddmap:
  - vregionaddmap(&myproc()->vspace.regions[VR_HEAP], &(myproc()->vspace.regions[VR_HEAP].va_base + myproc()->vspace.regions[VR_HEAP].size), n, VPI_PRESENT, VPI_WRITABLE);

### growustack
- Make helper function in `kernel/trap.c`: `int growustack()` and call on tf->trapno == TRAP_PF check 
  - call vregionaddmap(). If it fails, return -1.
  - vspace.regions[VR_USTACK].size += PGSIZE;
  - check to make sure that the stack will not grow to exceed 10 pages

  - call for vregionaddmap:
    - vregionaddmap(&myproc()->vspace.regions[VR_USTACK], &(myproc()->vspace.regions[VR_USTACK].va_base - myproc()->vspace.regions[VR_USTACK].size), PGSIZE, VPI_PRESENT, VPI_WRITABLE);

### copy-on-write fork
- have a field in core_map_entry in inc/mmu.h that keeps track of the reference count to 
  a specific physical page
- have a field in vpage_info that indicates whether the page is copy-on-write or not
- need to handle synchronization, as both the child and the parent can attempt to write 
  to the same page at the same time

### changes to vspacecopy
- go through the vregions as usual
- for each vregion go through all of its vpi_page, and for every vpi_page go through its 
  vpage_info (size = VPIPAGE) and grab the ppn
- call pa2page(vpi->ppn << PT_SHIFT) which returns a core_map_entry
- increment the reference count of the core_map_entry
NOTE: struct vpi_page *pages in struct vregion is a linked list

### changes to fork
- set writable in vspace_info to indicate the page is read only after forking
- set variable on vspace_info to indicate that the page is copy-on-write?
NOTE: modify kfree to only free if the ref count == 1

### copypage
- A helper function in `kernel/trap.c`: `int copypage()` and call on tf->trapno == TRAP_PF
  - if (ref_count (from core_table_entry) == 1 and writable == false and COW == true)
    - use the call va2vregion with myproc()->vspace and addr we get from rcr2() (see trap.c:trap) 
      to get a vregion (is the addr we get from rcr2() the address of the current page?)
    - call va2vpage_info with the vregion we get and addr to get a vpage_info, call it vpi for now
    - call pa2page(vpi.ppn << PT_SHIFT) to get the core_map_entry
    - increment the reference count in the core_map_entry that we just get
    - call vregionaddmap (sz = 4096)
    - call memmove (copies the page into the new page)
  - if (ref_count (from core_table_entry) == 1 and writable == false and COW == true), no need 
    to allocate a new page
NOTE: need to edit current core_table_entry struct to have a ref count and current vpage_info 
struct to have int cow


### System Calls

kernel/sysproc.c: sys_sbrk
- Call `int argint(int n, int *ip)` to parse arg0 which is the integer value of amount of 
  memory to be added to the heap
- Error cases to check and potential fixes:
- If there is insufficient space to allocate the heap (if value returned from sbrk(int n) 
  is negative) return -1.

### Running xk shell and commands
- Change config so that we can run the already implemented xk shell
- If commands do not run as expected, debug to find bugs in previously implemented code 
  - Priority one: exec, pipe, and sbrk
  - Priority two: file functions
NOTE: change pipe such that it returns 0 if reading with a closed write end.


## Risk Analysis

### Unanswered Questions
- When a page fault occurs, how do we know whether it is because we need to grow the stack 
  of it is because of copy-on-write fork?
- Which lock should we use to handle the synchronization in copy-on-write fork?
- Where exactly is the critical section that we need to use lock on to handle the case where 
  the parent and the child process are trying to write to the same page at the same time?
- Where do we need to call vspaceinvalidate() within the operations for on-demand stack 
  growth and copy-on-write fork? 

### Staging of Work (Time Estimation)
- sys_sbrk(): 2-3 hours 
- Starting shell: 1-2 hours
- On-demand stack grow: 3-4 hours
- Copy-on-write fork: 4-5 hours
- Doing short answer questions: 1.5-2.5 hours

