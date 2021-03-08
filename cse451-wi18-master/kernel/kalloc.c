// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include <cdefs.h>
#include <defs.h>
#include <e820.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <spinlock.h>
#include <proc.h>
#include <fs.h>
#include <sleeplock.h>
#include <buf.h>

int npages = 0;
// LAB 4
int spages = 0;
// LAB 4
int pages_in_use;
int pages_in_swap;
int free_pages;

struct core_map_entry *core_map = NULL;
// LAB 4
struct swap_entry swap_map[2048];
int is_vspaceinvalidating;
// LAB 4

struct core_map_entry *pa2page(uint64_t pa) {
  if (PGNUM(pa) >= npages) {
    cprintf("%x\n", pa);
    panic("pa2page called with invalid pa");
  }
  return &core_map[PGNUM(pa)];
}

struct swap_entry *getswapentry(uint64_t spn) {
  return &swap_map[spn];
}

uint64_t page2pa(struct core_map_entry *pp) {
  return (pp - core_map) << PT_SHIFT;
}

// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

void detect_memory(void) {
  uint32_t i;
  struct e820_entry *e;
  size_t mem = 0, mem_max = -KERNBASE;

  e = e820_map.entries;
  for (i = 0; i != e820_map.nr; ++i, ++e) {
    if (e->addr >= mem_max)
      continue;
    mem = max(mem, (size_t)(e->addr + e->len));
  }

  // Limit memory to 256MB.
  mem = min(mem, mem_max);
  npages = mem / PGSIZE;
  cprintf("E820: physical memory %dMB\n", mem / 1024 / 1024);
}

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file

struct {
  struct spinlock lock;
  int use_lock;
} kmem;

struct {
  struct sleeplock lock;
} swap_lock;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void mem_init(void *vstart) {
  void *vend;

  core_map = vstart;
  memset(vstart, 0, PGROUNDUP(npages * sizeof(struct core_map_entry)));
  vstart += PGROUNDUP(npages * sizeof(struct core_map_entry));

  for (int i = 0; i < npages; i++) {
    initlock(&core_map[i].lock, "core_map_entry lock");
  }

  memset(swap_map, 0, PGROUNDUP(2048 * sizeof(struct swap_entry)));
  for (int i = 0; i < 2048; i++) {
    swap_map[i].available = 1;
  }
  initsleeplock(&swap_lock.lock, "swap lock");

  is_vspaceinvalidating = 0;

  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;

  vend = (void *)P2V((uint64_t)(npages * PGSIZE));
  freerange(vstart, vend);
  free_pages = (vend - vstart) >> PT_SHIFT;
  pages_in_use = 0;
  pages_in_swap = 0;
  kmem.use_lock = 1;
}

void freerange(void *vstart, void *vend) {
  char *p;
  p = (char *)PGROUNDUP((uint64_t)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(char *v) {
  struct core_map_entry *r;

  if ((uint64_t)v % PGSIZE || v < _end || V2P(v) >= (uint64_t)(npages * PGSIZE))
    panic("kfree");

  if (kmem.use_lock)
    acquire(&kmem.lock);

  r = (struct core_map_entry *) pa2page(V2P(v));

  acquire(&r->lock);

  // There is 1 or less pointers to this page. Delete page.
  if (r->ref_count <= 1) {

    pages_in_use--;
    free_pages++;

    // Fill with junk to catch dangling refs.
    memset(v, 2, PGSIZE);

    r->available = 1;
    r->user = 0;
    r->va = 0;
    r->ref_count = 0;

  // There are multiple pointers to this page. Decrement count.
  } else {
    r->ref_count--;
  }

  release(&r->lock);
  
  if (kmem.use_lock) {
    release(&kmem.lock);
  }

}

void
mark_user_mem(uint64_t pa, uint64_t va)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 1;
  r->va = va;
}

void
mark_kernel_mem(uint64_t pa)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 0;
  r->va = 0;
}

uint64_t getavailablespn() {
  for (int i = 0; i < 2048; i++) {
    if (swap_map[i].available == 1) {
      
      initlock(&swap_map[i].lock, "swap_map lock");
      acquire(&swap_map[i].lock);
      swap_map[i].available = 0;
      release(&swap_map[i].lock);
      return i;
    }
  }
  return 0;
}


int diskread(uint64_t va, uint64_t spn) {
  for (int i = 0; i < 8; i++) {
    uchar mem[BSIZE];
    
    struct buf *buf = bread(ROOTDEV, spn * 8 + i + 2);
    memmove(mem, buf->data, BSIZE);
    brelse(buf);
    memmove((void*)va + 512 * i, mem, BSIZE);
  }
  acquiresleep(&swap_lock.lock);
  pages_in_swap--;
  releasesleep(&swap_lock.lock);
  return 0;
}


int diskwrite(uint64_t va, uint64_t spn) {
  for (int i = 0; i < 8; i++) {
    struct buf *buf = bread(ROOTDEV, spn * 8 + i + 2);
    memmove(buf->data, (void*)va + 512 * i, BSIZE);
    bwrite(buf);
    brelse(buf);
  }
  pages_in_swap++;
  return 0;
}


void swapin(uint64_t spn, uint64_t va, uint64_t dst) {
  diskread(dst, spn);

  acquiresleep(&swap_lock.lock);
  updatevpages(va, PGNUM(V2P(dst)), VPI_PRESENT, spn, 0);
  releasesleep(&swap_lock.lock);
}


int evict_i = 0;

uint64_t lru_evict() {
  evict_i++;
  uint64_t i = 0;

  while (1) {
    int index = (i + evict_i) % npages;
    if (core_map[index].available == 0
        && core_map[index].va != 0
        && core_map[index].user != 0
        && vawasaccessed(&myproc()->vspace, core_map[index].va) == 0) {
      evict_i = index;
      return index;
    }
    release(&core_map[index].lock);
    evict_i++;
  }

  return 0;
}


char *kalloc(void) {
  while (free_pages < 10 && is_vspaceinvalidating == 0) {
    // Get an available spn
    uint64_t spn = getavailablespn();

    // Get cme we want to evict
    uint64_t i  = lru_evict();
    struct core_map_entry *cme = &core_map[i];

    // Populate swap_map[spn]
    acquire(&swap_map[spn].lock);
    swap_map[spn].user = cme->user;
    swap_map[spn].va = cme->va;
    swap_map[spn].ref_count = cme->ref_count;
    release(&swap_map[spn].lock);

    // Copy page to disk.
    acquiresleep(&swap_lock.lock);
    diskwrite((uint64_t) P2V(i << PT_SHIFT), spn);
    releasesleep(&swap_lock.lock);

    // Set up for kfree
    acquire(&cme->lock);
    cme->ref_count = 0;
    release(&cme->lock);

    // Free the page which is now in disk.
    kfree(P2V(i << PT_SHIFT));

    // Update the vpages.
    updatevpages(swap_map[spn].va, i, 0, spn, VPI_SWAP);

    pages_in_use++;
    free_pages--;

    if (kmem.use_lock)
      acquire(&kmem.lock);
    acquire(&core_map[i].lock);
    core_map[i].available = 0;
    core_map[i].ref_count = 1;
    release(&core_map[i].lock);
    if (kmem.use_lock)
      release(&kmem.lock);

    return P2V(page2pa(&core_map[i]));
  }

  int i;

  if (kmem.use_lock)
    acquire(&kmem.lock);
  for (i = 0; i < npages; i++) {
    acquire(&core_map[i].lock);
    if (core_map[i].available == 1) {
      core_map[i].available = 0;
      core_map[i].ref_count = 1;
      release(&core_map[i].lock);

      pages_in_use++;
      free_pages--;

      if (kmem.use_lock)
        release(&kmem.lock);
      return P2V(page2pa(&core_map[i]));
    }
    release(&core_map[i].lock);
  }

  if (kmem.use_lock)
    release(&kmem.lock);
  return 0;
}
