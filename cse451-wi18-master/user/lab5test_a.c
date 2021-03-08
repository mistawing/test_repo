#include <cdefs.h>
#include <fcntl.h>
#include <fs.h>
#include <memlayout.h>
#include <param.h>
#include <stat.h>
#include <syscall.h>
#include <trap.h>
#include <user.h>

char buf[8192];
int stdout = 1;

#define error(msg, ...)                                                        \
  do {                                                                         \
    printf(stdout, "ERROR (line %d): ", __LINE__);                             \
    printf(stdout, msg, ##__VA_ARGS__);                                        \
    printf(stdout, "\n");                                                      \
    exit();                                                                    \
    while (1) {                                                                \
    };                                                                         \
  } while (0)

#define assert(a)                                                              \
  do {                                                                         \
    if (!(a)) {                                                                \
      printf(stdout, "Assertion failed (line %d): %s\n", __LINE__, #a);        \
      while (1)                                                                \
        ;                                                                      \
    }                                                                          \
  } while (0)

void modification(void) {
  int fd;

  printf(stdout, "modification test starting\n");
  strcpy(buf, "lab5 is 451's last lab.\n");
  fd = open("small.txt", O_RDWR);
  write(fd, buf, 50);
  close(fd);

  fd = open("small.txt", O_RDONLY);
  read(fd, buf, 50);

  if (strcmp(buf, "lab5 is 451's last lab.\n") != 0)
    error("file content was not lab5 is 451's last lab., was: '%s'", buf);

  close(fd);

  printf(stdout, "modification test ok!\n");
}

void onefile(void) {
  int fd, i, j;
  printf(1, "one file test\n");

  if ((fd = open("onefile.txt", O_CREATE|O_RDWR)) < 0)
    error("create 'onefile.txt' failed");

  memset(buf, 0, sizeof(buf));
  for (i = 0; i < 10; i++) {
    memset(buf, i, 512);
    write(fd, buf, 512);
  }
  close(fd);
  if ((fd = open("onefile.txt", O_RDONLY)) < 0)
    error("couldn't reopen 'onefile.txt'");

  memset(buf, 0, sizeof(buf));
  for (i = 0; i < 10; i++) {
    if (read(fd, buf, 512) != 512)
      error("couldn't read the bytes for iteration %d", i);
    for (j = 0; j < 512; j++)
      assert(i == buf[j]);
  }

  printf(1, "one file test passed\n");
}

// four processes write different files at the same
// time, to test block allocation.
void fourfiles(void) {
  int fd, pid, i, j, n, total, pi;
  char *names[] = {"f0", "f1", "f2", "f3"};
  char *fname;

  printf(1, "fourfiles test\n");

  for (pi = 0; pi < 4; pi++) {
    fname = names[pi];

    pid = fork();
    if (pid < 0) {
      error("fork failed\n");
    }

    if (pid == 0) {
      fd = open(fname, O_CREATE | O_RDWR);
      if (fd < 0) {
        error("create failed\n");
      }

      memset(buf, '0' + pi, 512);
      for (i = 0; i < 12; i++) {
        if ((n = write(fd, buf, 500)) != 500) {
          error("write failed %d\n", n);
        }
      }
      exit();
    }
  }

  for (pi = 0; pi < 4; pi++) {
    wait();
  }

  for (i = 0; i < 4; i++) {
    fname = names[i];
    fd = open(fname, 0);
    total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      for (j = 0; j < n; j++) {
        if (buf[j] != '0' + i) {
          error("wrong char, was %d should be %d\n", buf[j], '0' + i);
        }
      }
      total += n;
    }
    close(fd);
    if (total != 12 * 500) {
      error("wrong length %d\n", total);
    }
  }

  printf(1, "fourfiles ok\n");
}

int main(int argc, char *argv[]) {
  printf(stdout, "lab5test_a starting\n");
  modification();
  onefile();
  fourfiles();

  printf(stdout, "lab5test_a passed!\n");
  exit();
}
