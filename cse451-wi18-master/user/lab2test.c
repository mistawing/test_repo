#include <cdefs.h>
#include <fcntl.h>
#include <stat.h>
#include <stdarg.h>
#include <user.h>

#define error(msg, ...)                                                        \
  do {                                                                         \
    printf(stdout, "ERROR (line %d): ", __LINE__);                             \
    printf(stdout, msg, ##__VA_ARGS__);                                        \
    printf(stdout, "\n");                                                      \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

// After error functionality is tested, this is just a convenience
#define assert(a)                                                              \
  do {                                                                         \
    if (!(a)) {                                                                \
      printf(stdout, "Assertion failed (line %d): %s\n", __LINE__, #a);        \
      while (1)                                                                \
        ;                                                                      \
    }                                                                          \
  } while (0)

int stdout = 1;

void forktest(void);
void pipetest(void);
void pkilltest(void);
void fdesctest(void);
void childpidtest(void);
void exectest(void);

int main() {
  int pid, wpid;

  if (open("console", O_RDWR) < 0) {
    return -1;
  }
  dup(0); // stdout
  dup(0); // stderr

  pid = fork();
  if (pid < 0) {
    printf(1, "fork failed\n");
    exit();
  }

  if (pid == 0) {
    forktest();
    pipetest();
    fdesctest();
    childpidtest();
    exectest();

    printf(1, "lab2 tests passed!!\n");

    while (1)
      ;
  }

  while ((wpid = wait()) >= 0 && wpid != pid)
    printf(1, "zombie!\n");

  exit();
  return 0;
}

void forktest(void) {
  int n, pid;
  int nproc = 6;

  printf(1, "forktest\n");

  for (n = 0; n < nproc; n++) {
    pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      exit();
      error("forktest: exit failed to destroy this process");
    }
  }

  if (n != nproc) {
    error("forktest: fork claimed to work %d times! but only %d\n", nproc, n);
  }

  for (; n > 0; n--) {
    if (wait() < 0) {
      error("forktest: wait stopped early\n");
    }
  }

  if (wait() != -1) {
    error("forktest: wait got too many\n");
  }

  printf(1, "forktest: fork test OK\n");
}

void pipetest(void) {
  char buf[500];
  int fds[2], pid;
  int seq, i, n, cc, total;

  if (pipe(fds) != 0) {
    error("pipetest: pipe() failed\n");
  }
  pid = fork();
  seq = 0;
  if (pid == 0) {
    close(fds[0]);
    for (n = 0; n < 5; n++) {
      for (i = 0; i < 95; i++)
        buf[i] = seq++;
      if (write(fds[1], buf, 95) != 95) {
        error("pipetest: oops 1\n");
      }
    }
    exit();
  } else if (pid > 0) {
    close(fds[1]);
    total = 0;
    cc = 1;
    while ((n = read(fds[0], buf, cc)) > 0) {
      for (i = 0; i < n; i++) {
        if ((buf[i] & 0xff) != (seq++ & 0xff)) {
          error("pipetest: oops 2\n");
        }
      }
      total += n;
      cc = cc * 2;
      if (cc > sizeof(buf))
        cc = sizeof(buf);
    }
    if (total != 5 * 95) {
      error("pipetest: oops 3 total %d\n", total);
    }
    close(fds[0]);
    wait();
  } else {
    error("pipetest: fork() failed\n");
  }
  printf(1, "pipetest ok\n");
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

void fdesctest(void) {
  printf(1, "fdesctest\n");
  int fd1;
  char buf[11] = {0};

  // Make sure offsets are shared
  assert((fd1 = open("l2_share.txt", O_RDONLY)) > -1);

  if (!fork()) {
    if (read(fd1, buf, 10) != 10)
      error("tried to read 10 bytes from fd1");

    if (strcmp("cccccccccc", buf))
      error("should have reead 10 c's, instead: '%s'", buf);
    exit();
  } else {
    wait();
  }

  if (read(fd1, buf, 10) != 10)
    error("tried to read 10 bytes from fd1");

  if (strcmp("ppppppppp\n", buf))
    error("should have reead 9 p's (and a newline), instead: '%s'", buf);

  close(fd1);

  // Make sure the correct file descriptor ints are maintained

  assert(open("l2_share.txt", O_RDONLY) > -1);
  assert(open("l2_share.txt", O_RDONLY) > -1);
  assert(open("l2_share.txt", O_RDONLY) > -1);
  assert(open("l2_share.txt", O_RDONLY) > -1);

  assert(close(4) != -1);
  assert(close(5) != -1);

  if (!fork()) {
    assert(read(3, buf, 10) == 10);
    assert(read(4, buf, 10) == -1); // this fd shouldn't be open
    assert(read(5, buf, 10) == -1); // this fd shouldn't be open
    assert(read(6, buf, 10) == 10); // this fd shouldn't be open
    exit();
  } else {
    wait();
  }

  printf(1, "fdesctest: passed\n");
}

static void childpidtesthelper(int depth) {
  int pids[3];
  int i, j, p;

  if (depth == 0)
    return;

  for (i = 0; i < 3; i++) {
    pids[i] = fork();
    assert(pids[i] >= 0);
    if (!pids[i]) {
      childpidtesthelper(depth - 1);
      exit();
    }
  }

  for (i = 0; i < 3; i++) {
    p = wait();
    assert(p > 0);
    for (j = 0; j < 3; j++) {
      if (p == pids[j]) {
        pids[j] = -1; // make sure duplicates are not returned
        break;
      }
    }
    if (j == 3)
      error("returned pid was not a child");
  }

  // No more children
  if (wait() != -1)
    error("there should be no more children");
}

void childpidtest() {
  printf(1, "childpidtest\n");
  childpidtesthelper(2);
  printf(1, "childpidtest: passed\n");
}

void exectest(void) {
  printf(1, "exectest: starting ls\n");
  int pid = fork();
  if (pid < 0) {
    error("exectest: fork failed\n");
  }

  char *argv[] = {0};

  if (pid == 0) {
    exec("ls", argv);
    error("exectest: exec ls failed\n");
  } else {
    pid = wait();
  }

  char *echoargv[] = {"echo", "echotest", "ok", 0};
  printf(stdout, "exectest: test argument\n");

  int fds[2];
  assert(pipe(fds) == 0);

  pid = fork();
  if (pid < 0) {
    error("exectest: fork failed\n");
  }

  if (pid == 0) {
    // send output to parent.
    close(stdout);
    assert(dup(fds[1]) == stdout);
    exec("echo", echoargv);
    error("exectest: exec echo failed\n");
  } else {
    assert(wait() == pid);

    // read output from child.
    printf(stdout, "exectest: test output\n");
    char buf[16];
    char *expected = "echotest ok\n";
    int count = read(fds[0], buf, 15);
    if (count == -1)
      error("exectest: exec echotest no output");

    buf[count] = 0;
    if (strcmp(buf, expected) != 0) {
      printf(stdout, "exectest: bad output:\n");
      printf(stdout, buf);
      error("exectest: output test failed");
    }
    printf(stdout, expected);
  }

  printf(1, "exectest passed: ok\n");
}
