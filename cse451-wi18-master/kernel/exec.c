#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <trap.h>
#include <x86_64.h>
#include <x86_64vm.h>

int exec(int n, char *path, char **argv) {

  struct vspace temp;
  vspaceinit(&temp);

  uint64_t rip;
  int size = vspaceloadcode(&temp, path, &rip);
  if (size == 0) {
    vspacefree(&temp);
    return -1;
  }

  int res;

  res = vspaceinitstack(&temp, SZ_2G);
  if (res < 0) {
    vspacefree(&temp);
    return -1;
  }

  // contains the address of the string arguments on the user stack
  // the 2 extra slots are for the null terminator between the arguments
  // and the argument pointers, and the address of return pc
  uint64_t ustack_args[n + 2];

  // value of the return pc does not matter
  ustack_args[0] = 0x0;
  ustack_args[n + 1] = 0;


  // write the string arguments to the user stack, and keep track of
  // the address of each string arguments in the user stack
  uint64_t va = SZ_2G;
  for (int i = 0; i < n; i++) {
    // decrement address pointer
    uint64_t size = strlen(argv[i]) + 1 + 8; // 8 to ceil
    va -= (size / 8) * 8;
    // write data into the current address, return -1 if it fails
    res = vspacewritetova(&temp, va, argv[i], strlen(argv[i]) + 1);
    if (res < 0) {
      vspacefree(&temp);
      return -1;
    }
    // add argument address to user stack
    ustack_args[1 + i] = va;
  }

  // go to the address where we will write the ustack_args to the user stack
  va -= (2 + n) * 8;

  // write the pointers to the string arguments in the user stack to the
  // user stack, along with a null terminator and a garbage return pc
  if (vspacewritetova(&temp, va, (char*)ustack_args, (2 + n) * 8) < 0) {
    vspacefree(&temp);
    return -1;
  }

  // set up the registers
  myproc()->tf->rip = rip;
  myproc()->tf->rsi = va + 8;
  myproc()->tf->rdi = n;
  myproc()->tf->rsp = va;

  // free vregion, but save a ptr to the pgtbl
  pml4e_t *pgtbl = myproc()->vspace.pgtbl;
  vregionfree(&(myproc()->vspace));
  vspaceinit(&myproc()->vspace); 
  vspaceinstall(myproc());

  // copy temp into the current vspace
  res = vspacecopy(&(myproc()->vspace), &temp);
  
  // now free the pgtbl and the temp
  vspacefree(&temp);
  freevm(pgtbl);
  if (res < 0)
    return -1;

  return 0;
}
