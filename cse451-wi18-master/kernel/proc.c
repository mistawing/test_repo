#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>
#include <fs.h>
#include <file.h>
#include <vspace.h>

extern int is_vspaceinvalidating;

// process table
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// to test crash safety in lab5, 
// we trigger restarts in the middle of file operations
void reboot(void) {
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
loop:
  asm volatile("hlt");
  goto loop;
}

void pinit(void) { initlock(&ptable.lock, "ptable"); }

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void) {
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0) {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trap_frame *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 8;
  *(uint64_t *)sp = (uint64_t)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)forkret;

  return p;
}

// Set up first user process.
void userinit(void) {
  struct proc *p;
  extern char _binary_out_initcode_start[], _binary_out_initcode_size[];

  p = allocproc();

  initproc = p;
  assertm(vspaceinit(&p->vspace) == 0, "error initializing process's virtual address descriptor");
  vspaceinitcode(&p->vspace, _binary_out_initcode_start, (int64_t)_binary_out_initcode_size);
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FLAGS_IF;
  p->tf->rip = VRBOT(&p->vspace.regions[VR_CODE]);  // beginning of initcode.S
  p->tf->rsp = VRTOP(&p->vspace.regions[VR_USTACK]);

  safestrcpy(p->name, "initcode", sizeof(p->name));

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void) {
  // your code here
  struct proc *p;

  p = allocproc();
  if (p == 0) {
    return -1;
  }

  assertm(vspaceinit(&p->vspace) == 0, "error in fork vspaceint");
  assertm(vspaceshallowcopy(&p->vspace, &myproc()->vspace) == 0, "error forking");
  vspaceinvalidate(&myproc()->vspace);
  vspaceinstall(myproc());

  // copy parent trap frame into child trap frame
  memmove(p->tf, myproc()->tf, sizeof(struct trap_frame));

  // copy parent proc's file table, updating ref count
  for (int i = 0; i < NOFILE; i++) {
    acquire(&ptable.lock);
    if (myproc()->file_table[i] != NULL) {

      p->file_table[i] = &(*(myproc()->file_table[i]));
      p->file_table[i]->ref_count++;     

    }
    release(&ptable.lock);
  }

  // set child proc as runnable and return values for each proc
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->parent = myproc();
  p->tf->rax = 0;

  release(&ptable.lock);

  return p->pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void) {
  // close all files on this process
  for (int i = 0; i < NOFILE; i++) {
    if (myproc()->file_table[i] != NULL) {
      fileclose(myproc(), myproc()->file_table[i], i);
    }
  }

  // wakeup parent, setting child state to zombie, and scheduling it
  acquire(&ptable.lock);
  // search through proc table for any children for this proc.
  // if any, change parent to initproc and wake up initproc
  for (int i = 0; i < NPROC; i++) {
    if (ptable.proc[i].parent->pid == myproc()->pid) {
      ptable.proc[i].parent = initproc;
      if (initproc->state == SLEEPING)
        wakeup1(initproc);
    }
  }
  wakeup1(myproc()->parent);
  myproc()->state = ZOMBIE;
  sched();

  release(&ptable.lock);

}

// Looks through the proc table for a zombie child
// if found, reclaims the resources and returns the child pid
// if not found, returns -1.
int process_zombie_child(void) {
  for (int i = 0; i < NPROC; i++) {

     if (ptable.proc[i].parent->pid == myproc()->pid &&
         ptable.proc[i].state == ZOMBIE) {

       int child_pid = ptable.proc[i].pid;

       for (int j = 0; j < NOFILE; j++) {
         if (ptable.proc[i].file_table[j] != NULL) {
           fileclose(&ptable.proc[i], ptable.proc[i].file_table[j], i);
         }
       }
       ptable.proc[i].state = UNUSED;
       vspacefree(&ptable.proc[i].vspace);
       kfree(ptable.proc[i].kstack);
       ptable.proc[i].kstack = 0;
       ptable.proc[i].parent = 0;
       ptable.proc[i].pid = 0;
       ptable.proc[i].killed = 0;
       release(&ptable.lock);
       return child_pid;
    }
  }

  return -1;
}


// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void) {
  // check if there are any children
  int has_child = 0;
  for (int i = 0; i < NPROC; i++) {
    acquire(&ptable.lock);

    if (ptable.proc[i].parent->pid == myproc()->pid &&
        ptable.proc[i].state != UNUSED) {
      has_child = 1;
    }

    release(&ptable.lock);
  }

  // error - no children for this proc
  if (!has_child) {
    return -1;
  }

  // look for a zombie child, when found reclaim resources
  // and return the child pid; if not found, go to sleep.
  acquire(&ptable.lock);
  while (1) {
    int child_pid = process_zombie_child();
    if (child_pid > 0) {
      return child_pid;
    }

    sleep(myproc(), &ptable.lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void) {
  struct proc *p;

  for (;;) {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      mycpu()->proc = p;
      vspaceinstall(p);
      p->state = RUNNING;
      swtch(&mycpu()->scheduler, p->context);
      vspaceinstallkern();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      mycpu()->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void) {
  int intena;

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1) {
    cprintf("pid : %d\n", myproc()->pid);
    cprintf("ncli : %d\n", mycpu()->ncli);
    cprintf("intena : %d\n", mycpu()->intena);

    panic("sched locks");
  }
  if (myproc()->state == RUNNING)
    panic("sched running");
  if (readeflags() & FLAGS_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&myproc()->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void) {
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    //log_recover();
    iinit(ROOTDEV);
    //log_recover();
  }

  // Return to "caller", actually trapret (see allocproc).
}

// allocates more memory on the heap
int sbrk(int n) {
  // check if current "unused" space inside the user heap can already
  // support the given number of bytes
  struct vspace myvspace = myproc()->vspace;
  uint64_t old_heap_bound = myvspace.regions[VR_HEAP].va_base + myvspace.regions[VR_HEAP].size;

  int res = vregionaddmap(&myproc()->vspace.regions[VR_HEAP], 
                          old_heap_bound,
                          n,
                          VPI_PRESENT,
                          VPI_WRITABLE);
  if (res < 0) { 
    return  -1;
  }

  myproc()->vspace.regions[VR_HEAP].size += n;
  vspaceinvalidate(&myproc()->vspace);
  return old_heap_bound;
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) {
  if (myproc() == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock) { // DOC: sleeplock0
    acquire(&ptable.lock);  // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  myproc()->chan = chan;
  myproc()->state = SLEEPING;
  sched();

  // Tidy up.
  myproc()->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock) { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan) {
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan) {
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid) {
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

uint64_t updatevpages(uint64_t va, uint64_t ppn, short present, uint64_t spn, short swap) {
  struct proc *p;

  uint64_t prev_pn = 0;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    for (int i = VR_CODE; i < VR_USTACK; i++) {
      struct vregion *vr = va2vregion(&p->vspace, va);  
      if (!vr || !vregioncontains(vr, va, 1))
        continue;
      struct vpage_info *vpi = va2vpage_info(vr, va);

      // moving this page back to p mem
      if (present == VPI_PRESENT && vpi->spn == spn) {
        prev_pn = vpi->spn;
        vpi->ppn = ppn;
        vpi->present = present;
        vpi->spn = 0;
        vpi->swap = 0;

        is_vspaceinvalidating = 1;
        vspaceinvalidate(&p->vspace);
        is_vspaceinvalidating = 0;

      // moving this to swap region
      } else if (swap == VPI_SWAP && vpi->ppn == ppn) {
        prev_pn = vpi->ppn;
        vpi->spn = spn;
        vpi->swap = swap;
        vpi->ppn = 0;
        vpi->present = 0;

        is_vspaceinvalidating = 1;
        vspaceinvalidate(&p->vspace);
        is_vspaceinvalidating = 0;

      }
    }
  }

  return prev_pn;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
  static char *states[] = {[UNUSED] = "unused",   [EMBRYO] = "embryo",
                           [SLEEPING] = "sleep ", [RUNNABLE] = "runble",
                           [RUNNING] = "run   ",  [ZOMBIE] = "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint64_t pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state != 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING) {
      getcallerpcs((uint64_t *)p->context->rbp, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

struct proc *findproc(int pid) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid)
      return p;
  }
  return 0;
}
