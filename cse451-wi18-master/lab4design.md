#Lab 4 Design Doc: Virtual Memory
Group members: Hae In Lee, Lemei Zhang

## Overview
The main objective of this lab is to implement least-recently used (LRU) page swap. When the physical memory limitation is reached, pages should be saved onto the disk in order of LRU and reloaded when they are needed. 

### Major Parts

Modify system setup: We will reduce the setup of QEMU to 4 MB of physical memory and implement a swap region of 8 MB. In order to make our system behave as if it has 12 MB of physical memory given the new changes, we will have to add a swap space/region to the xk’s hard disk. The swap space should be located between the bitmap and super block with a size of 1024 * 8 blocks. Since each page is equivalent to the size of 8 blocks, this region should be able to hold 1024 pages. Foreseen challenges to implementing this swap region includes designing a simple and effective means of bookkeeping memory, data, and pointers and maintaining the functionality of the current system.

Swapping to disk: We will implements calls `diskread` and `diskwrite` to read and write the disk, respectively. This will allow us to swap physical pages to and from disk. Things we have to consider in this section include: timing of flushing pages to the swap region and reloading them from the disk, bookkeeping memory pages in the swap region, swapped memory pages that are also COW, exceptional pages that should not be flushed to the swapped region, forking when some pages are in the swap region, modifying current data structures so that they contain swap information, and exiting a process when some pages are in the swap region. 

Reducing number of disk operations: We will implement the least-recently used (second chance) algorithm, where we will swap memory page that is not recently used by some vspace to the disk.


## In-depth Analysis and Implementation

### Current files/functions/struct to modify
`mkfs.c`: add swap region to xk’s hard disk
- Add variable `uint swapstart` in `struct superblock`, which is located in fs.h
- Changes in main() of mkfs.c: 
	* swapsize = 1024 * 8 
	* sb.swapstart = xint(2)
	* sb.bmapstart = xint(2 + swapsize)
	* sb.inodestart = xint(2 + swapsize + nbitmap)

In inc/vspace.h `struct vpage_info`: update to indicate if page is in the swap region
#define VPI_SWAP ((short) 1): 1 indicating that this page is currently in the swap region
- `short swap`: add swap flag that indicates whether this page currently exists in the swap region. 
- `uint64_t spn`: add swap page number

In kernel/vspace.c `vspaceshallowcopy`: update to handle increasing reference count for page in swap region
- Check the swap flag
	* If swap flag == 1, go ahead and grab the corresponding swap_entry and increment its ref_count
	* Otherwise, grab the corresponding core_map_entry and increment its ref_count

In kernel/kalloc.c `kalloc`: update to support PAGE OUT swap
- if (pages_in_use == npages), the physical memory limitation has been reached
- Call lru_evict(&cme) to get the core_map_entry we plan to empty 
- uint64_t spn = 0;
- For each swap_entry 
	* If (swap_map[i].available == 1) 
		* swap_map[i] .available = 0;
		* spn = i;
		* Exit loop
- For each process 
	* if (vspacecontains(vspace, cme->va, PGSIZE))
	* For each region 
		* If (vregioncontains(vregion, cme->va, PGSIZE)
			* vpi = va2vpage_info(vregion, cme->va) 
			* vpi.present = 0;
			* vpi. Swap = 1;
			* vpi.spn = blockno;
- Grab lock
- `diskwrite(cme->va, spn)`
- `kfree` the page that was saved into disk 
- Continue with rest of kalloc()
- Release lock

In kernel/kalloc.c `kfree`: update to support freeing pages that may lie in the swap region
- vreg = va2vregion(&myproc()->vspace, v);
- vpi = va2vpage_info(vreg, v);
- If (vpi.swap == 1 && swap_map[vpi.spn].ref_count <= 1) 
	* Swap_map[vpi.spn].available = 1;
	* Swap_map[vpi.spn].user = 0;
	* Swap_map[vpi.spn].va = 0;
	* Swap_map[vpi.spn].ref_count = 0;
- Else if (vpi.swap == 1 && swap_map[vpi.spn].ref_count > 1)
	* swap_map[vpi.spn].ref_count--;  

In kernel/trap.c `trap`: update to support PAGE IN swap 
- If (vpi.swap == 1) we have a page fault on page that is currently in the swap region. 
- Uint64_t va = Kalloc() to get a new page in physical memory 
- Grab lock
- `swapout(vpi.spn, va)`
- Release lock

### New structs/ data structures
In inc/mmu.h `struct swap_entry`:
- `int available`
- `short user`
- `uint64_t va`
- `int ref_count`
- `struct spinlock lock`

In kernel/kalloc.c `struct swap_entry *disk_map = NULL;`

### Functions
`swapout(uint spn, uint64_t va)`: removes corresponding swap entry and updates all the vpageinfos 
* Swap_map[spn].available = 1; // remove swap_entry 
* For each process 
	* if (vspacecontains(vspace, va, PGSIZE))
		* For each region 
			* If (vregioncontains(vregion, va, PGSIZE)
				* vpi = va2vpage_info(vregion, va) 
				* Vpi.present = 1;
				* vpi. Swap = 0;
				* Vpi.spn = 0;
				* Vpi.ppn = PGNUM(V2P(va));
- diskread(va, vpn);

`int diskread(uint64_t va, uint64_t spn)`: returns 0 on success and -1 on failure.
- For (int i = 0; i < 8; i ++)
	* Struct buf mem;
	* `bread(ROOTDEV, spn * 8 + i)`: returns a locked buf with the contents of the indicated block defined by the device number and block_no (both uint)
	* `memmove(mem, buf->data, BSIZE)`: moves buf->data into mem
	* `brelse(buf)`: (aka block release) decrements the reference count and deallocate the in-memory disk block when its reference count drops to 0.
	* `memmove(va + 512 * i, mem->data, BSIZE)`
- `pages_in_swap--`: decrement number of pages in swap.

`int diskwrite(uint64_t va, uint64_t spn)`: returns 0 on success and -1 on failure.
- For (int i = 0; i < 8; i++)
	* `struct buf *buf = bread(ROOTDEV, spn * 8 + i)`: returns a locked buf with the contents of the indicate block defined by the device number and block_no (both uint); if the block is already in the cache, it will read the block from the cache. Else, it loads the disk block from disk into the buffer cache.
	* `memmove(buf->data, va + 512 * i, BSIZE)`: moves BSIZE bytes at ph_addr into buf->data
	* `bwrite(buf)`: flushes the data in a buf to disk
	* `brelse(buf)`: (aka block release) decrements the reference count and deallocate the in-memory disk block when its reference count drops to 0.
- `pages_in_swap++`: increment number of pages in swap.

In `kernel/kalloc.c`: `struct core_map_entry *lru_evict()`:
- Go through every core_map_entry with size npages
- if (cme->va == 0), the page is part of the kernel, so skip it
- Otherwise, call vawasaccessed in vspace.c to check whether the page was recently accessed or not
	* If so, set its accessed bit to 0 (which should be handled by vspacewasaccessed(vspace, cme->va))
	* Otherwise, return the core map entry, and we can do the page in/page out handling operations as needed 

### Modular tests
- To test everything after part 2; you can use a dummy swap function that swaps by looping through.

## Risk Analysis

### Unanswered Questions
- How do we update all the vpage_infos when we add something to the swap region?
- Synchronization

### Staging of Work (Time Estimation)
- Add the swap region to xks hard disk: 0.5 hours 
- Implement swapping physical pages to and from disk: 6 - 7 hours 
- Implement LRU page swap: 4 - 5 hours
- Short answer questions: 2.5 - 3 hours


