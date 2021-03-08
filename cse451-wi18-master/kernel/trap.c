#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

int growustack(void) {
  struct vspace myvspace = myproc()->vspace;
  
  // If the stack is currently larger than or equal to 
  // ten pages, return error.
  if (myvspace.regions[VR_USTACK].size >= 10 * PGSIZE) {
    return -1;
  }

  int old_stack_bound = myvspace.regions[VR_USTACK].va_base 
                        - myvspace.regions[VR_USTACK].size - PGSIZE;
  // Try adding a new page to the user stack.
  int res = vregionaddmap(&myproc()->vspace.regions[VR_USTACK],
                old_stack_bound,
                PGSIZE,
                VPI_PRESENT,
                VPI_WRITABLE);
  if (res < 0) {
    return -1;
  }
  myproc()->vspace.regions[VR_USTACK].size += PGSIZE;

  vspaceinvalidate(&myproc()->vspace);

  return old_stack_bound;
}


void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      struct vregion *vreg;
      struct vpage_info *vpi;
      if ((vreg = va2vregion(&myproc()->vspace, addr)) != 0
          && (vpi = va2vpage_info(vreg, addr)) != 0) {

        if (vpi->present == 0 && vpi->swap == VPI_SWAP) {

          // Get the se of this page
          struct swap_entry *se = getswapentry(vpi->spn);
          // Get a free page
          char *mem = kalloc();
          if (!mem) {
            break;
          }

          // Reset its contents
          memset(mem, 0, PGSIZE);

          // Get the cme of this page
          struct core_map_entry *cme = pa2page(V2P(mem));

          // Populate the cme
          acquire(&cme->lock);
          cme->user = se->user;
          cme->va = (uint64_t) se->va;
          cme->ref_count = se->ref_count;
          release(&cme->lock);

          // Swap in data (reads and updates the vpages)
          swapin(vpi->spn, (uint64_t) addr, (uint64_t) mem);

          // Reset se
          acquire(&se->lock);
          se->available = 1;
          release(&se->lock);
          break;

        } else {
          struct core_map_entry *cme = pa2page(vpi->ppn << PT_SHIFT);
        
      
          // Multiple references to an unwritable page. 
          // Make a copy and set writable to true. 
          if (cme->ref_count > 1 && vpi->writable == 0 && vpi->cow == 1) {
            cme->user = 0;

            // Allocate a physical page, and copy the data in the current
            // physical page to the new physical page
            char *mem = kalloc();
            if (!mem) {
              break;
            }

            acquire(&cme->lock);

            memset(mem, 0, PGSIZE);
            memmove(mem, P2V(vpi->ppn << PT_SHIFT), PGSIZE);

            // Make the vpi that caused the page fault point to the new 
            // physical page
            vpi->used = 1;
            vpi->present = VPI_PRESENT;
            vpi->writable = VPI_WRITABLE;
            vpi->cow = 0;
            vpi->swap = 0;
            vpi->ppn = PGNUM(V2P(mem));

            // decrement the reference count of the old physical page
            cme->ref_count--;

            cme->user = 1;

            vspaceinvalidate(&myproc()->vspace);
            vspaceinstall(myproc());

            release(&cme->lock);
            break;

          // Last reference to this page. Set back to writable.
          } else if (cme->ref_count == 1 && vpi->writable == 0 && vpi->cow == 1) {
            acquire(&cme->lock);

            cme->user = 0;

            vpi->writable = VPI_WRITABLE;
            vpi->cow = 0;
            vpi->swap = 0;
            vspaceinvalidate(&myproc()->vspace);
            vspaceinstall(myproc());

            cme->user = 1;

            release(&cme->lock);
            break;

          }
        }  
      }

      if (addr < SZ_2G && addr >= SZ_2G - 10 * PGSIZE) {
        if (growustack() != -1) {
          break;
        }
      }

      if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}
