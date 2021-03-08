#include <cdefs.h>
#include <fcntl.h>
#include <stat.h>
#include <stdarg.h>
#include <sysinfo.h>
#include <user.h>

#define error(msg, ...)                                                        \
  do {                                                                         \
    printf(stdout, "ERROR (line %d): ", __LINE__);                             \
    printf(stdout, msg, ##__VA_ARGS__);                                        \
    printf(stdout, "\n");                                                      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

#define KERNBASE 0xFFFFFFFF80000000

int stdout = 1;

void racetest(void);
void pkilltest(void);
void memtest(void);
void sbrktest(void);
void growstacktest(void);
void copyonwriteforktest(void);

int main(int argc, char *argv[]) {
  racetest();
  pkilltest();
  memtest();
  sbrktest();
  growstacktest();
  copyonwriteforktest();

  printf(stdout, "lab3 tests passed!!\n");

  exit();
  return 0;
}

void racetest(void) {
  int i, pid;

  for (i = 0; i < 100; i++) {
    pid = fork();
    if (pid < 0) {
      error("racetest: fork failed\n");
    }
    if (pid) {
      if (wait() != pid) {
        error("racetest: wait wrong pid\n");
      }
    } else {
      exit();
    }
  }
  printf(1, "racetest ok\n");
}

void pkilltest(void) {
  char buf[11];

  int pid1, pid2, pid3;
  int pfds[2];

  printf(1, "preempt: ");
  pid1 = fork();
  if (pid1 == 0)
    for (;;)
      ;

  pid2 = fork();
  if (pid2 == 0)
    for (;;)
      ;

  pipe(pfds);
  pid3 = fork();
  if (pid3 == 0) {
    close(pfds[0]);
    if (write(pfds[1], "x", 1) != 1)
      error("pkilltest: write error");
    close(pfds[1]);
    for (;;)
      ;
  }

  close(pfds[1]);
  if (read(pfds[0], buf, sizeof(buf)) != 1) {
    error("pkilltest: read error");
  }
  close(pfds[0]);
  printf(1, "kill... ");
  kill(pid1);
  kill(pid2);
  kill(pid3);
  printf(1, "wait... ");
  wait();
  wait();
  wait();
  printf(1, "pkilltest: ok\n");
}

void memtest() {
  void *m1, *m2;
  int pid;
  int i;

  printf(stdout, "memtest\n");
  if ((pid = fork()) == 0) {
    m1 = 0;
    for (i = 0; i < 10; i++) {
      m2 = malloc(10001);
      *(char **)m2 = m1;
      m1 = m2;
    }
    while (m1) {
      m2 = *(char **)m1;
      free(m1);
      m1 = m2;
    }
    m1 = malloc(1024 * 20);
    if (m1 == 0) {
      error("couldn't allocate mem?!!\n");
    }
    free(m1);
    printf(stdout, "mem ok\n");
    exit();
  } else {
    wait();
  }
  printf(stdout, "memtest passed\n");
}

void sbrktest(void) {
  int fds[2], pid, pids[7];
  char *a, *b, *c, *lastaddr, *oldbrk, *p, scratch;
  uint64_t amt;

  printf(stdout, "sbrktest\n");
  oldbrk = sbrk(0);

  // can one sbrk() less than a page?
  a = sbrk(0);
  int i;
  for (i = 0; i < 5000; i++) {
    b = sbrk(1);
    if (b != a) {
      error("sbrk test failed %d %x %x\n", i, a, b);
    }
    *b = 1;
    a = b + 1;
  }
  pid = fork();
  if (pid < 0) {
    error("sbrk test fork failed\n");
  }
  c = sbrk(1);
  c = sbrk(1);
  if (c != a + 1) {
    error("sbrk test failed post-fork\n");
  }
  if (pid == 0)
    exit();
  wait();

  // can one grow address space to something big?
#define BIG (1 * 1024 * 1024)
  a = sbrk(0);
  amt = (BIG) - (uint64_t)a;
  p = sbrk(amt);
  if (p != a) {
    error("sbrk test failed to grow big address space; enough phys mem?\n");
  }
  lastaddr = (char *)(BIG - 1);
  *lastaddr = 99;

  // can we read the kernel's memory?
  for (a = (char *)(KERNBASE); a < (char *)(KERNBASE + 2000000); a += 50000) {
    pid = fork();
    if (pid < 0) {
      error("fork failed\n");
    }
    if (pid == 0) {
      printf(stdout, "oops could read %x = %x\n", a, *a);
      error("a bad process is not killed!");
    }
    wait();
  }

  // if we run the system out of memory, does it clean up the last
  // failed allocation?
  if (pipe(fds) != 0) {
    error("pipe() failed\n");
  }
  for (i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
    if ((pids[i] = fork()) == 0) {
      // allocate a lot of memory
      sbrk(BIG - (uint64_t)sbrk(0));
      write(fds[1], "x", 1);
      // sit around until killed
      for (;;)
        sleep(1000);
    }
    if (pids[i] != -1)
      read(fds[0], &scratch, 1);
  }

  // if those failed allocations freed up the pages they did allocate,
  // we'll be able to allocate here
  c = sbrk(4096);
  for (i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
    if (pids[i] == -1)
      continue;
    kill(pids[i]);
    wait();
  }
  if (c == (char *)0xffffffff) {
    error("failed sbrk leaked memory\n");
  }

  if (sbrk(0) > oldbrk)
    sbrk(-(sbrk(0) - oldbrk));

  printf(stdout, "sbrktest OK\n");
}

void growstacktest() {
  int i;
  struct sys_info info1, info2;
  printf(stdout, "growstacktest\n");
  sysinfo(&info1);

  printf(stdout, "pages_in_use before stack allocation = %d\n",
         info1.pages_in_use);
  printf(stdout, "pages_in_swap before stack allocation = %d\n",
         info1.pages_in_swap);

  int page8 = 8 * 4096;
  char buf[page8];
  for (i = 0; i < 8; i++) {
    buf[i * 4096] = 'a';
    buf[i * 4096 - 1] = buf[i * 4096];
  }
  sysinfo(&info2);

  printf(stdout, "pages_in_use after stack allocation = %d\n",
         info2.pages_in_use);
  printf(stdout, "pages_in_swap after stack allocation = %d\n",
         info2.pages_in_swap);

  // if grow ustack on-demand is implemented, then the 8 pages are allocated at
  // run-time
  if (info2.pages_in_use - info1.pages_in_use + info1.pages_in_swap -
          info2.pages_in_swap !=
      8)
    error("user stack is not growing");

  printf(stdout, "growstacktest passed\n");
}

void copyonwriteforktest() {
  struct sys_info info1, info2, info3, info4;
  int page200 = 200 * 4096;
  char *a = sbrk(page200);
  char b;
  int j;

  a[0] = 0;
  printf(stdout, "copyonwriteforktest\n");
  sysinfo(&info1);
  printf(stdout, "pages_in_use before copy-on-write fork = %d\n",
         info1.pages_in_use);
  printf(stdout, "pages_in_swap before copy-on-write fork = %d\n",
         info1.pages_in_swap);
  int pid = fork();
  if (pid == 0) {
    sleep(100);
    exit();
  } else {
    sysinfo(&info2);
    printf(stdout, "pages_in_use after copy-on-write fork = %d\n",
           info2.pages_in_use);
    printf(stdout, "pages_in_swap after copy-on-write fork = %d\n",
           info2.pages_in_swap);

    // if copy-on-write is implemented, there is no way a new process can take
    // more than 100 pages
    if (info2.pages_in_use - info1.pages_in_use + info1.pages_in_swap -
            info2.pages_in_swap >
        100)
      error("too much memory is used for fork");

    for (j = 0; j < 200; j++) {
      b = a[j * 4096];
    }

    sysinfo(&info3);
    printf(stdout, "pages_in_use after read = %d\n", info3.pages_in_use);
    printf(stdout, "pages_in_swap after read = %d\n", info3.pages_in_swap);

    // Read should not increase the amount of memory allocated
    if (info3.pages_in_use - info2.pages_in_use + info3.pages_in_swap -
            info2.pages_in_swap >
        100)
      error("too much memory is used for read");

    for (j = 0; j < 200; j++) {
      a[j * 4096] = b;
    }

    sysinfo(&info4);
    printf(stdout, "pages_in_use after write = %d\n", info4.pages_in_use);
    printf(stdout, "pages_in_swap after write = %d\n", info4.pages_in_swap);

    // Write should allocate the 200 pages of memory
    if (info4.pages_in_use - info3.pages_in_use + info4.pages_in_swap -
            info3.pages_in_swap <
        100)
      error("too less memory is used for write");

    wait();
  }

  printf(stdout, "copyonwriteforktest passed\n");
}
