#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <vspace.h>
#include <proc.h>
#include <x86_64.h>
#include <x86_64vm.h>

// Gets the index into the linked table of vpi_pages.
static int
va2vpi_idx(struct vregion *r, uint64_t va)
{
  if (!r)
    panic("null vregion");
  if (r->dir == VRDIR_UP)
    return (va - r->va_base) >> PAGE_SHIFT;
  else if (r->dir == VRDIR_DOWN)
    return (r->va_base - 1 - va) >> PAGE_SHIFT;
  panic("va2vpi_idx: invalid direction");
}

// Creates the architecture specific page permision bits.
static int
x86perms(struct vpage_info *vpi)
{
  int perms = PTE_U; // always user if in a virtual region
  if (vpi->present)
    perms |= PTE_P;
  if (vpi->writable)
    perms |= PTE_W;
  return perms;
}

extern pml4e_t *kpml4;

// To be called at initialization time only. Will set up the
// kernel page table and the segment table.
void
vspacebootinit(void)
{
  kpml4 = setupkvm();
  vspaceinstallkern();
  seginit();   // segment table
}

// Should be called before any vspace functions are used on a vspace.
// This will map the kernel to the upper portion of the address space,
// leave the user space unmapped.
int
vspaceinit(struct vspace *vs)
{
  struct vregion *vr;
  // TODO MAYBE: allocate a starter page for all the mem_regions,
  // or maybe just do that on demand
  if (!(vs->pgtbl = setupkvm()))
    return -1;

  for (vr = vs->regions; vr < &vs->regions[NREGIONS]; vr++) {
    memset(vr, 0, sizeof(struct vregion));
  }

  vs->regions[VR_CODE].dir   = VRDIR_UP;
  vs->regions[VR_HEAP].dir   = VRDIR_UP;
  vs->regions[VR_USTACK].dir = VRDIR_DOWN;

  return 0;
}

// Adds a BLANK (newly allocated) page to the page table in the bounds [from_va, sz).
// The present bit should be set if the page is to be marked present.
// The writable bit indicates whether the page is writable for the user.
int
vregionaddmap(struct vregion *vr, uint64_t from_va, uint64_t sz, short present, short writable)
{
  char *mem;
  uint64_t a;
  struct vpage_info *vpi;

  if (sz + from_va >= KERNBASE)
    return -1;
  if (sz <= 0)
    return 0;

  for (a = PGROUNDUP(from_va); a < from_va + sz; a += PGSIZE) {
    if (!(vpi = va2vpage_info(vr, a)))
      return -1;

    mem = kalloc();
    if (!mem)
      return -1;
    memset(mem, 0, PGSIZE);

    vpi->used = 1;
    vpi->present = present;
    vpi->writable = writable;
    vpi->ppn = PGNUM(V2P(mem));
  }
  return sz;
}

// Will remove the mapping from a vregion and free all pages in the
// virtual address range (from_va - size, from_va]
int
vregiondelmap(struct vregion *vr, uint64_t from_va, uint64_t sz)
{
  uint64_t a;
  struct vpage_info *vpi;

  if (!vregioncontains(vr, from_va - sz, 0))
    return -1;
  if (sz <= 0)
    return 0;

  for (a = PGROUNDDOWN(from_va); a > from_va - sz; a -= PGSIZE) {
    if (!(vpi = va2vpage_info(vr, a)))
      return -1;

    assertm(vpi->used, "address isn't assigned");

    vpi->used = 0;
    vpi->present = 0;
    vpi->writable = 0;
    kfree(P2V(vpi->ppn << PT_SHIFT));
  }
  return sz;
}

// Maps data from [data, data + sz) into [va, va + sz)
static int
vradddata(struct vregion *r, uint64_t va, char *data, int sz, short present, short writable)
{
  int ret;
  uint64_t i, n;
  struct vpage_info *vpi;

  if ((ret = vregionaddmap(r, va, sz, present, writable)) < 0)
    return ret;

  for (i = 0; i < sz; i += PGSIZE) {
    vpi = va2vpage_info(r, va + i);
    assert(vpi->used);
    n = min((uint64_t)sz - i, (uint64_t)PGSIZE);
    memmove(P2V(vpi->ppn << PT_SHIFT), data + i, n);
  }
  return 0;
}

// Loads data from an inode into [va, va + sz)
// va must be page aligned
static int
vrloaddata(struct vregion *r, uint64_t va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  struct vpage_info *vpi;
  assertm(va % PGSIZE == 0, "va must be page aligned");

  for (i = 0; i < sz; i += PGSIZE) {
    vpi = va2vpage_info(r, va + i);
    assertm(vpi->used, "page must be allocated");
    n = min(sz - i, (uint) PGSIZE);
    if (readi(ip, P2V(vpi->ppn << PT_SHIFT), offset + i, n) != n)
      return -1;
  }

  return 0;
}

// Loads the initialization code. This should only be called with the
// first processes, as it doesn't set up enough stack space or leave
// room for a heap.
//
// NOTE: The vspace is invalidated at the end, so the vspace is ready
// to be installed after calling this function.
void
vspaceinitcode(struct vspace *vs, char *init, uint64_t size)
{
  uint64_t stack;

  // code pages
  vs->regions[VR_CODE].va_base = 0;
  vs->regions[VR_CODE].size = PGROUNDUP(size);
  assertm(
    vradddata(&vs->regions[VR_CODE], 0, init, size, VPI_PRESENT, VPI_WRITABLE) == 0,
    "failed to allocate init code data"
  );

  // add the stack
  // make room for the stack and (implied) guard
  stack = PGROUNDUP(size) + PGSIZE;

  vs->regions[VR_USTACK].va_base = stack;
  vs->regions[VR_USTACK].size = PGSIZE;
  assert(
    vregionaddmap(&vs->regions[VR_USTACK], stack - PGSIZE, PGSIZE, VPI_PRESENT, VPI_WRITABLE) >= 0
  );

  vspaceinvalidate(vs);
}

// This will load the code into a vspace from a file found by path.
// The return parameter rip is the point where the program should start
// executing.
//
// NOTE: The vspace is not invalidated (Due to the likelihood of of a vspace being
// changed more before it is to be installed). You must invalidate before installing.
int
vspaceloadcode(struct vspace *vs, char *path, uint64_t *rip)
{
  struct inode *ip;
  struct proghdr ph;
  int off, sz;
  uint64_t va;
  struct elfhdr elf;
  int i;

  if((ip = namei(path)) == 0){
    return 0;
  }

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto elf_failure;
  if(elf.magic != ELF_MAGIC)
    goto elf_failure;

  // Set start bound
  vs->regions[VR_CODE].va_base = 0;

  // Load program into memory.
  va = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto elf_failure;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto elf_failure;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto elf_failure;

    if((sz = vregionaddmap(&vs->regions[VR_CODE], (uint64_t)va, ph.vaddr + ph.memsz, VPI_PRESENT, VPI_WRITABLE)) < 0)
     goto elf_failure;
    if(ph.vaddr % PGSIZE != 0)
      goto elf_failure;

    va += sz;

    if(vrloaddata(&vs->regions[VR_CODE], ph.vaddr, ip, ph.off, ph.filesz) < 0)
     goto elf_failure;
  }

  // Set end bound;
  vs->regions[VR_CODE].size = PGROUNDUP(sz);
  // The heap will be right after the code
  vs->regions[VR_HEAP].va_base = PGROUNDUP(sz);
  vs->regions[VR_HEAP].size = 0;

  irelease(ip);
  *rip = elf.entry;
  return sz;
elf_failure:
  if(ip)
    irelease(ip);

  return 0;
}

// This will do the necessary processing to transform a vspace
// into the architecture dependent page table. Must be called after
// any changes are made that affect the mappings in a vspace.
void
vspaceinvalidate(struct vspace *vs)
{
  struct vregion *vr;
  struct vpage_info *vpi;
  pte_t *pte;
  uint64_t start, end;
  int accessed;

  for (vr = vs->regions; vr < &vs->regions[NREGIONS]; vr++) {
    start = VRBOT(vr);
    end = VRTOP(vr);

    assert(start % PGSIZE == 0);

    for (; start < end; start += PGSIZE) {
      vpi = va2vpage_info(vr, start);
      pte = walkpml4(vs->pgtbl, (char *)start, 0);
      accessed = 0;
      if (pte) {
        if (vpi->used)
          accessed = *pte & PTE_A;
        *pte = 0;
      }
      if (vpi->present) {
        mappages(vs->pgtbl, start >> PT_SHIFT, 1, vpi->ppn, x86perms(vpi)|accessed, 0);
        accessed = 0;
      }
    }
  }
}

// Given a vspace (must be initialized) install the page table into
// the page table register. Since this function doesn't invalidate,
// any changes since the last install must be `vspcaeinvalidate`d
void
vspaceinstall(struct proc *p)
{
  if (!p)
    panic("mrinstall: null proc");
  if (!p->kstack)
    panic("mrinstall: null kstack");
  if (!p->vspace.pgtbl)
    panic("mrinstall: page table not initialized");

  pushcli();
  mycpu()->ts.rsp0 = (uint64_t)p->kstack + KSTACKSIZE;
  lcr3(V2P(p->vspace.pgtbl));
  popcli();
}

// Installs the kernel page table.
void
vspaceinstallkern(void)
{
  lcr3(V2P(kpml4));
}

// Frees the vpi_page linked table.
static void
free_page_desc_list(struct vpi_page *page)
{
  assert((uint64_t) page % PGSIZE == 0);

  if (!page)
    return;

  struct vpage_info *vpi;
  for (vpi = &page->infos[0]; vpi < &page->infos[VPIPPAGE]; vpi++) {
    if (vpi->swap == VPI_SWAP && vpi->present == 0) {
      struct swap_entry *se = getswapentry(vpi->spn);
      acquire(&se->lock);

      if (se->ref_count <= 1) {

        pages_in_swap--;

        se->available = 1;
        se->user = 0;
        se->va = 0;
        se->ref_count = 0;

      } else {
        se->ref_count--;
      }

      release(&se->lock);
    }
  }

  free_page_desc_list(page->next);
  kfree((char *)page);
}

void 
vregionfree(struct vspace *vs) {
  struct vregion *vr;

  for (vr = &vs->regions[0]; vr < &vs->regions[NREGIONS]; vr++) {
    free_page_desc_list(vr->pages);
    memset(vr, 0, sizeof(struct vregion));
  }

}

// Frees all the memory a vspace is consuming.
// A vspace must be reinitialized if it is to be used again.
void
vspacefree(struct vspace *vs)
{
  struct vregion *vr;

  for (vr = &vs->regions[0]; vr < &vs->regions[NREGIONS]; vr++) {
    free_page_desc_list(vr->pages);
    memset(vr, 0, sizeof(struct vregion));
  }

  freevm(vs->pgtbl);
}

// Given a virtual address return the region that contains it.
// will return 0 if no region is found.
struct vregion*
va2vregion(struct vspace *vs, uint64_t va)
{
  struct vregion *vr;

  for (vr = &vs->regions[0]; vr < &vs->regions[NREGIONS]; vr++) {
    if (vr->dir == VRDIR_UP) {
      if (va >= vr->va_base && va < vr->va_base + vr->size)
        return vr;
    } else {
      if (va >= vr->va_base - vr->size && va < vr->va_base)
        return vr;
    }
  }
  return 0;
}

// Given a vregion and virtual address, find the page info struct
// associated with that virutal address.
struct vpage_info*
va2vpage_info(struct vregion *vr, uint64_t va)
{
  int idx;
  struct vpi_page *info;

  if (!vr->pages) {
    vr->pages = (struct vpi_page *)kalloc();
    memset(vr->pages, 0, PGSIZE);
  }

  idx = va2vpi_idx(vr, va);
  info = vr->pages;

  while (idx >= VPIPPAGE) {
    assertm(info, "idx was out of bounds");
    if (!info->next) {
      info->next = (struct vpi_page *)kalloc();
      if (!info->next)
        return 0;
      memset(info->next, 0, PGSIZE);
    }
    info = info->next;
    idx -= VPIPPAGE;
  }

  return &info->infos[idx];
}

// Tests if a vregion has [va, va + size) mapped in it's virtual address space.
int
vregioncontains(struct vregion *vr, uint64_t va, int size)
{
  return va >= VRBOT(vr) && va + size < VRTOP(vr);
}

// Tests if a vspace contains [va, va + size).
int
vspacecontains(struct vspace *vs, uint64_t va, int size)
{
  struct vregion *vr = va2vregion(vs, va);
  if (!vr)
    return -1;
  return vregioncontains(vr, va, size);
}

// Does a linked list style copy of the values.
static int
copy_vpi_page(struct vpi_page **dst, struct vpi_page *src)
{
  int i;
  char *data;
  struct vpage_info *srcvpi, *dstvpi;

  if (!src) {
    *dst = 0;
    return 0;
  }

  if (!(*dst = (struct vpi_page *)kalloc()))
    return -1;

  memset(*dst, 0, sizeof(struct vpi_page));

  for (i = 0; i < VPIPPAGE; i++) {
    srcvpi = &src->infos[i];
    dstvpi = &(*dst)->infos[i];
    if (srcvpi->used) {
      dstvpi->used = srcvpi->used;
      dstvpi->present = srcvpi->present;
      dstvpi->writable = srcvpi->writable;
      dstvpi->swap = 0;
      if (!(data = kalloc()))
        return -1;
      memmove(data, P2V(srcvpi->ppn << PT_SHIFT), PGSIZE);
      dstvpi->ppn = PGNUM(V2P(data));
    }
  }

  return copy_vpi_page(&(*dst)->next, src->next);
}

static int
shallow_copy_vpi_page(struct vpi_page **dst, struct vpi_page *src)
{
  int i;
  struct vpage_info *srcvpi, *dstvpi;

  if (!src) {
    *dst = 0;
    return 0;
  }

  if (!(*dst = (struct vpi_page *)kalloc()))
    return -1;

  memset(*dst, 0, sizeof(struct vpi_page));

  // Copy vpage_infos and set writable to false.
  for (i = 0; i < VPIPPAGE; i++) {
    srcvpi = &src->infos[i];
    dstvpi = &(*dst)->infos[i];

    if (srcvpi->used) {
      dstvpi->used = srcvpi->used;
      dstvpi->present = srcvpi->present;

      // set cow to true
      dstvpi->cow = srcvpi->cow = VPI_COW;

      // set writable to false.
      dstvpi->writable = srcvpi->writable = 0;

      // Copy ppn and update ref_count.
      dstvpi->ppn = srcvpi->ppn;
      dstvpi->spn = srcvpi->spn;
      dstvpi->swap = srcvpi->swap;
     
     // if page is in swap, update swap entry ref_count 
     if (srcvpi->swap == VPI_SWAP) {
 
        struct swap_entry *se = getswapentry(srcvpi->spn);
        acquire(&se->lock);
        se->ref_count++;
        release(&se->lock); 
      // else, update cme ref_count
      } else {
        struct core_map_entry *cme = pa2page(srcvpi->ppn << PT_SHIFT);
        acquire(&cme->lock);
        cme->ref_count++;
        release(&cme->lock);
      }
    }
  }

  return shallow_copy_vpi_page(&(*dst)->next, src->next);

}

// Copies all the mappings from src to dst. While the virtual addresses
// are the same and the data is the same, the mapped pages are
// disjoint. Aka a write to one va in the src is not reflected in the dst
// virtual address space and vice versa.
int
vspacecopy(struct vspace *dst, struct vspace *src)
{
  struct vregion *vr;

  memmove(dst->regions, src->regions, sizeof(struct vregion) * NREGIONS);

  for (vr = dst->regions; vr < &dst->regions[NREGIONS]; vr++)
    if (copy_vpi_page(&vr->pages, vr->pages) < 0)
      return -1;

  vspaceinvalidate(dst);

  return 0;
}

int
vspaceshallowcopy(struct vspace *dst, struct vspace *src)
{
  struct vregion *vr;

  memmove(dst->regions, src->regions, sizeof(struct vregion) * NREGIONS);

  for (vr = dst->regions; vr < &dst->regions[NREGIONS]; vr++)
    if (shallow_copy_vpi_page(&vr->pages, vr->pages) < 0)
      return -1;

  vspaceinvalidate(dst);

  return 0;
}

// Initializes the user stack at start.
//
// NOTE: Invalidates the vspace, so can be directly installed after
// this function returns.
int
vspaceinitstack(struct vspace *vs, uint64_t start)
{
  struct vregion *vr = &vs->regions[VR_USTACK];
  vr->va_base = start;
  vr->size = PGSIZE;

  // stack page
  if (vregionaddmap(vr, start - PGSIZE, PGSIZE, VPI_PRESENT, VPI_WRITABLE) < 0)
    return -1;

  vspaceinvalidate(vs);

  return 0;
}

// Copies out [data, data + size) to [va, va + size). This is very useful
// to "put" data in a page table that is not currently installed.
int
vspacewritetova(struct vspace *vs, uint64_t va, char *data, int sz)
{
  uint64_t end, wsz;
  struct vpage_info *vpi;
  struct vregion *vr;

  assertm(sz > 0, "sz less than or equal to 0");
  assertm(va + sz < KERNBASE, "went over kernel vm base");

  end = va + sz;
  while (va < end) {
    wsz = min((int)(PGROUNDUP(va) - va), sz);

    if (!(vr = va2vregion(vs, va)))
      return -1;

    vpi = va2vpage_info(vr, va);
    assert(vpi->used);

    if (!vpi->writable)
      return -1;

    memmove(P2V(vpi->ppn << PT_SHIFT) + (va % PGSIZE), data, wsz);

    va += wsz;
    data += wsz;
    sz -= wsz;
  }

  return 0;
}

int
vawasaccessed(struct vspace *vs, uint64_t va)
{
  pte_t *pte;
  int accessed;

  pte = walkpml4(vs->pgtbl, (char *)va, 0);
  if (!pte || !(*pte & PTE_P))
    return 0;

  accessed = *pte & PTE_A;
  *pte &= ~PTE_A;

  return accessed;
}
