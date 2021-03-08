#include <cdefs.h>
#include <fcntl.h>
#include <fs.h>
#include <memlayout.h>
#include <stat.h>
#include <sysinfo.h>
#include <user.h>

int stdout = 1;

#define error(msg, ...)                                                        \
  do {                                                                         \
    printf(stdout, "ERROR (line %d): ", __LINE__);                             \
    printf(stdout, msg, ##__VA_ARGS__);                                        \
    printf(stdout, "\n");                                                      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

#define START_PAGES (600)
#define SWAP_TEST_PAGES (START_PAGES * 2)

void swaptest(void) {
  char *start = sbrk(0);
  char *a;
  int i;
  int b = 4096;
  int num_pages_to_alloc = SWAP_TEST_PAGES;
  struct sys_info info1, info2, info3;

  if (!fork()) {
    for (i = 0; i < num_pages_to_alloc; i++) {
      a = sbrk(b);
      if (a == (char *)-1) {
        printf(stdout, "no more memory\n");
        break;
      }
      memset(a, 0, b);
      *(int *)a = i;
      if (i % 100 == 0)
        printf(stdout, "%d pages allocated\n", i);
    }

    sysinfo(&info1);

    // check whether memory data is consistent
    for (i = 0; i < num_pages_to_alloc; i++) {
      if (i % 100 == 0)
        printf(stdout, "checking i %d\n", i);
      if (*(int *)(start + i * b) != i) {
        error("data is incorrect, should be %d, but %d\n", i,
              *(int *)(start + i * b));
      }
    }

    sysinfo(&info2);

    printf(stdout, "number of disk reads = %d\n",
           info2.num_disk_reads - info1.num_disk_reads);

    sysinfo(&info3);
    printf(stdout, "number of pages in swap = %d\n", info3.pages_in_swap);

    printf(stdout, "swaptest OK\n");
    exit();
  } else {
    wait();
  }
}

void localitytest(void) {
  char *start = sbrk(0);
  char *a;
  int i, j, k;
  int b = 4096;
  int groups = 6;
  int pages_per_group = SWAP_TEST_PAGES / groups;
  struct sys_info info1, info2, curinfo, previnfo;

  sysinfo(&info1);
  for (i = 0; i < groups * pages_per_group; i++) {
    a = sbrk(b);
    memset(a, 0, b);
    *(int *)a = i;
  }

  printf(stdout, "%d pages allocated\n", groups * pages_per_group);

  sysinfo(&info1);
  printf(stdout, "number of pages in swap = %d\n", info1.pages_in_swap);

  int reads = 0;
  int prevreads;
  int curdiskreads;
  // test whether a rough approximation of LRU is implemented
  for (i = 0; i < groups; i++) {
    sysinfo(&previnfo);
    prevreads = reads;
    for (j = groups - 1; j >= i; j--) {
      for (k = pages_per_group - 1; k >= 0; k--) {
        reads++;
        if (*(int *)(start + (j * pages_per_group + k) * b) !=
            j * pages_per_group + k) {
          error("data is incorrect");
        }
      }
    }
    sysinfo(&curinfo);
    curdiskreads = curinfo.num_disk_reads - previnfo.num_disk_reads;
    if (i == 0) {
      // On the first iteration, we should only incur about half the page faults
      // (the lower portion of the groups should be in swap)
      if (curdiskreads >= (SWAP_TEST_PAGES - 2 * pages_per_group) * 8 * 2)
        error("On first iteration, there should have been fewer disk reads");
    }
    if (i == groups-1) {
      // There should be no disk reads on the last one as the other groups have
      // been well least recently used
      if (curdiskreads != 0)
        error("The pages accessed on the last iteration were not all resident in memory");
    }
    printf(1, "iteration %d, total disk reads %d, this iteration disk reads %d\n",
        i, curinfo.num_disk_reads, curinfo.num_disk_reads - previnfo.num_disk_reads);
  }

  // if LRU is implemented,
  // this will reduce the number of disk reads to less than 230000

  // If LRU is not implemented, assuming page swap at every memory access
  // Number of disk operations is around (1200 + 1000 + 800 + 600 + 400 + 200) * 8 * 2 = 67000

  // If LRU is implemented, the first ~400 pages should not incur disk
  // operations Number of disk operations is around (800 + 1000 + 800 + 600) * 8 * 2 = 51200
  // = 88000

  // we set threshold to be 57600 so any LRU-like implementation can pass our
  // test


  sysinfo(&info2);

  if (info2.num_disk_reads - info1.num_disk_reads > 60000)
    error("LRU function incurs too many swaps.");

  printf(stdout, "localitytest OK\n");
}

int main(int argc, char *argv[]) {
  swaptest();
  localitytest();
  printf(stdout, "lab4 tests passed!!\n");
  exit();
}
