# Lab 4: Virtual Memory

## Introduction
Physical machines have limited amount of physical memory. To allow applications
(individually, or in aggregate) to consume more memory than this physical memory
limitation, the operating system uses a designated region on the disk to serve as an extended memory.
On reaching the physical memory limit, operating systems will start to flush memory pages to disk
and load them back when needed. In this lab, you are going to implement least-recently used (LRU) page swap.

## Part 1: Modify system setup

Currently, in QEMU (the hardware simulator), we set the amount of memory to be 16MB (4096
pages). When xk boots, it will show you how many memory pages are left. This number should
be between 3000 - 4000 depending on how memory efficient your implementation is in the previous labs.
```
cpu0: starting xk

free pages: 3601
```
You can always query the amount of free pages using `sysinfo`. When memory pages are fully
utilized, `kalloc` will return 0.

We're going to bring down the physical memory demands and add some paging so we can run on
a device with very restricted physical memory demands. First in `kernel/Makefrag` change the line:
```
QEMUOPTS += -m 16M
```
to
```
QEMUOPTS += -m 4M
```

This sets up QEMU with 4MB of physical memory. We ask you to implement a swap region of 8MB
(2048 pages). Our expectation with your implementation is that the system should behave
as if it has more than 4MB of physical memory, up to the 12MB of combined physical and swap
memory. We will not test scenarios when the system runs out
of the swap space (in a real system one would need to correctly handle that case).


### Question #1
How is the `core_map` allocated? Is it through `kalloc`? Will the core_map ever be evicted to disk?

### Reserve swap space on disks
`mkfs.c` is a utility tool to generate the content on the hard disk. You need to modify it to add the swap section. Currently, xk's hard drive has a disk layout as the following:

```
	+------------------+  <- number of blocks in the hard drive
	|                  |
	|      Unused      |
	|                  |
	+------------------+  <- block 2 + nbitmap + size of inodes + size of extent
	|                  |
	|                  |
	|      Extent      |
	|                  |
	|                  |
	+------------------+  <- block 2 + nbitmap + size of inodes
	|                  |
	|      Inodes      |
	|                  |
	+------------------+  <- block 2 + nbitmap
	|      Bitmap      |
	+------------------+  <- block 2
	|   Super Block    |
	+------------------+  <- block 1
	|    Boot Block    |
	+------------------+  <- block 0
```

The best place to add the swap space is between the super block and the bitmap. You can
allocate 2048 * 8 blocks (each block is 512 bytes, you need 8 blocks to store a page of
memory) in the swap region.

### Exercise
Add the swap region to xk's hard disk through `mkfs.c`.

### Question #2
`mkfs.c` has functions like `xint`, `xshort`. What is their purpose?

## Part 2: Swapping to disk

You will use `bread` and `bwrite` to read and write the disk respectively. Take a look at
`struct buf` in `inc/buf.h`. To read data from a disk block, you can use code similar to
the snippet below:
```c
struct buf *buf = bread(dev, block_no);
memmove(mem, buf->data, BSIZE);
brelse(buf);
```
Here `block_no` should be the block number on disk. `buf` is the buffer that holds the disk
content. Each `bread` will read 512 bytes from disk. To write to a disk block, you will
need to use `bwrite`. For writes, you can use code similar to the following code snippet:
```c
struct buf *buf = bread(dev, block_no);
memmove(buf->data, P2V(ph_addr), BSIZE);
bwrite(buf);
brelse(buf);
```

The reason you need to do `bread`, `bwrite` and then `brelse` to write to a disk block is that
there is a buffer cache layer under the block level API (e.g., `bread`, `bwrite`).
The buffer cache is a list of disk blocks that are cached in memory to reduce the number
of disk writes. If the block is already in the cache, `bread` will read from block cache;
otherwise, it loads the disk block from disk into the buffer cache. `bwrite` flush the
data in a `buf` to disk. Because there can be multiple readers/writers to a buffer cache block,
the buffer cache layer has a reference count on any in-memory disk block. `brelse` (which stands for
"block release") decrements the reference count and deallocate the in-memory disk block
when its reference count drops to 0.

The interaction of virtual memory and the file buffer cache is one of the most complex
parts of operating systems. We designed the xk interface for simplicity -- a more realistic
implementation would avoid the initial read on page evictions.

### Question #3
What will happen if xk runs out of block cache entries?

### Keep track of a memory page's state

In order to implement the swap region, you need to think about a list of questions:
- When should we flush pages to the swap region and when should we load them back?
- How should we keep track of a memory page that is in swap region?
- What should happen when a swapped memory page is shared via copy-on-write fork?
- Is there a set of memory pages you don't want to flush to swap?
- What will happen when forking a process some of whose memory is in the swap region?
- How will the virtual page info/page table entry change for memory pages that are swapped out?
- What will happen when exiting a process some of whose memory is in the swap region?

You will need to define extra data structures in xk to realize those functionalities, such as
keeping track of the pages in the swap region. You can update the `struct vpage_info` to
contain information about the swap in a similar fashion to copy-on-write.

### Exercise
Implement swapping physical pages to and from disk.

### Question #4
xk guarantees that a physical memory page has a single virtual address even in the case of
multiple vspaces referencing it. However, in commercial operating systems, a memory mapping
function [`mmap`](http://man7.org/linux/man-pages/man2/mmap.2.html) can map a file to any
virtual address in mutliple address spaces. How would your design for swap need to change
if a shared page can have different virtual addresses? Give a rough outline of the changes
you would make.

## Part 3: Reducing number of disk operations
If your implementation is naive, you are going to run into an issue when
the system starts to page, where it will perform more evictions than necessary.
Our test cases will stress your system so that you will need to implement
some approximation to least-recently used (LRU).
The memory page swapped to the disk should be a page that is not recently used by some vspace.
This way, future reads and writes will be less likely to touch that on-disk memory page. Any reasonable approximation is likely to pass the tests.

We suggest you use the second chance least recently used approximation (clock) algorithm.
For this, the `vawasaccessed` function has been added to `vspace.c`. It will tell you if
the page was accessed, and then subsequently set the accessed bit to zero (giving it the
second chance to be accessed until the clock comes back around to check the page).

### Exercise
Implement LRU page swap.

### Question #5
For each member of the project team, how many hours did you
spend on this lab?

Create a `lab4.txt` file in the top-level xk directory with
your answers to the questions listed above.

When you're finished, create an `end_lab4` git tag as described above so we know the point at which you
submitted your code. In the root xk directory, run the turnin script:
```
$ make turnin
```
This will produce a `tar.gz` archive with your code. Upload this to [the lab 4 dropbox](
https://canvas.uw.edu/courses/1124225/assignments/4098106).
Zip your source code and and upload via dropbox.
