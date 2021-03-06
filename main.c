#define _XOPEN_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <limits.h>

#define WAIT_FOR_PROC_DEATH_TIMEOUT 10
#define SEP      "---"

#ifndef NDEBUG
#define DPRINTF(format, ...) fprintf(stderr, "smell-baron: " format "\n", ##__VA_ARGS__)
#else
#define DPRINTF(...)
#endif

// used as boolean by signal handlers to tell process it should exit
static volatile bool running = true;

typedef struct {
  /* args that form command, sent to execvp */
  char **args;

  /* see -f */
  bool watch;

  /* see -c */
  bool configuring;

  pid_t pid;
} Cmd;

typedef struct {
  /* number of commands to run */
  int n_cmds;

  /* all commands (watched and unwatched) */
  Cmd *cmds;
} ChildProcs;

typedef struct {
  bool signal_everything;
} Opts;

static int wait_for_requested_commands_to_exit(int n_watch_cmds, Cmd **watch_cmds) {
  int cmds_left = n_watch_cmds;
  int error_code = 0;
  int error_code_idx = INT_MAX;

  for (;;) {
    int status;

    if (! running) return error_code;
    pid_t waited_pid = waitpid(-1, &status, 0);
    if (waited_pid == -1) {
      if (errno == EINTR) {
        DPRINTF("waitpid interrupted by signal");
      }
      else {
        DPRINTF("waitpid returned unhandled error state");
      }
    }
    if (! running) return error_code;

    // check for pid in list of child pids
    for (int i = 0; i < n_watch_cmds; ++i) {
      if (watch_cmds[i]->pid == waited_pid) {
        if (WIFEXITED(status)) {
          int exit_status = WEXITSTATUS(status);
          DPRINTF("process exit with status: %d", exit_status);
          if (exit_status != 0 && i < error_code_idx) {
            error_code = exit_status;
            error_code_idx = i;
          }
        }

        DPRINTF("process exit: %d in command list, %d left", waited_pid, cmds_left - 1);

        if (--cmds_left == 0) {
          DPRINTF("all processes exited");
          return error_code;
        }
        break;
      }
      else if (i == n_watch_cmds - 1) {
        DPRINTF("process exit: %d not in watched commands list", waited_pid);
      }
    }
  }

  return error_code;
}

static int alarm_exit_code = 0;
static void wait_for_all_processes_to_exit(int error_code) {
  int status;
  alarm_exit_code = error_code;
  for (;;) {
    if (waitpid(-1, &status, 0) == -1 && errno == ECHILD)
      return;
  }
}

static pid_t run_proc(char **argv) {
  pid_t pid = fork();
  if (pid != 0)
    return pid;

  execvp(*argv, argv);
  fprintf(stderr, "failed to execute `%s'\n", *argv);
  exit(1);
}

static void on_signal(int signum) {
  DPRINTF("got signal %d", signum);
  running = false;
}

static void on_alarm(int signum) {
  DPRINTF("timeout waiting for child processes to die");
  exit(alarm_exit_code);
}

static void perror_die(char *msg) {
  perror(msg);
  exit(1);
}

static void install_term_and_int_handlers() {
  struct sigaction sa;
  sa.sa_handler = on_signal;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, NULL))
    perror_die("could not catch SIGINT");

  if (sigaction(SIGTERM, &sa, NULL))
    perror_die("could not catch SIGTERM");

  sa.sa_handler = on_alarm;
  if (sigaction(SIGALRM, &sa, NULL))
    perror_die("could not catch SIGALRM");
}

static void remove_term_and_int_handlers() {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGINT, &sa, NULL))
    return;
  if (sigaction(SIGTERM, &sa, NULL))
    return;
}

/**
 * Runs all commands marked with -c and waits for them to exit.
 */
static void run_configure_cmds(int n_cmds, Cmd *cmds) {
  int run_configure_cmds = 0;
  for (int i = 0; i < n_cmds; ++i) {
    if (! cmds[i].configuring)
      continue;

    cmds[i].pid = run_proc(cmds[i].args);
    ++run_configure_cmds;
  }

  if (run_configure_cmds) {
    DPRINTF("waiting for configuration commands to exit");
    int status;
    for (;;) {
      if (waitpid(-1, &status, 0) == -1 && errno == ECHILD)
        break;
    }
    DPRINTF("all configuration commands have exited");
  }
}

static void run_cmds(int n_cmds, Cmd *cmds) {
  for (int i = 0; i < n_cmds; ++i) {
    if (! cmds[i].configuring)
      cmds[i].pid = run_proc(cmds[i].args);
  }
}

static void parse_cmd_args(Opts *opts, Cmd *cmd, char **arg_it, char **args_end) {
  int c;
  optind = 1; // reset global used as pointer by getopt
  while ((c = getopt(args_end - arg_it, arg_it - 1, "+acf")) != -1) {
    switch(c) {
      case 'a':
        if (getpid() != 1) {
          fprintf(stderr, "-a can only be used from the init process (a process with pid 1)\n");
          exit(1);
        }
        opts->signal_everything = true;
        break;

      case 'c':
        cmd->configuring = true;
        break;

      case 'f':
        cmd->watch = true;
        break;
    }
  }

  if (cmd->configuring && cmd->watch) {
    DPRINTF("cannot use -c and -w together for a single command");
    exit(1);
  }

  cmd->args = arg_it + optind - 1;
}

static void parse_argv(ChildProcs *child_procs, Opts *opts, int argc, char *argv[]) {
  child_procs->n_cmds = 1;

  char **args_end = argv + argc;

  for (char **i = argv + 1; i < args_end; ++i) {
    if (! strcmp(*i, SEP))
      ++child_procs->n_cmds;
  }

  child_procs->cmds = calloc(child_procs->n_cmds, sizeof(Cmd));
  *opts = (Opts) { .signal_everything = false };
  parse_cmd_args(opts, child_procs->cmds, argv + 1, args_end);

  char **arg_it = child_procs->cmds->args;

  int cmd_idx = 0;
  for (; arg_it < args_end; ++arg_it) {
    if (! strcmp(*arg_it, SEP)) {
      *arg_it = 0; // replace with null to terminate when passed to execvp

      if (arg_it + 1 == args_end) {
        fprintf(stderr, "command must follow `---'\n");
        exit(1);
      }

      parse_cmd_args(opts, child_procs->cmds + (++cmd_idx), arg_it + 1, args_end);
      Cmd *cmd = child_procs->cmds + cmd_idx;
      arg_it = cmd->args - 1;
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc == 1) {
    fprintf(stderr, "please supply at least one command to run\n");
    return 1;
  }

  ChildProcs child_procs;
  Opts opts;
  parse_argv(&child_procs, &opts, argc, argv);

  int n_watch_cmds = 0;
  for (int i = 0; i < child_procs.n_cmds; ++i)  {
    if (child_procs.cmds[i].watch)
      ++n_watch_cmds;
  }

  // if -f hasn't been used then watch every command
  if (0 == n_watch_cmds) {
    for (int i = 0; i < child_procs.n_cmds; ++i) {
      if (! child_procs.cmds[i].configuring) {
        ++n_watch_cmds;
        child_procs.cmds[i].watch = true;
      }
    }
  }

  Cmd **watch_cmds = calloc(n_watch_cmds, sizeof(Cmd *));
  {
    int watch_cmd_end = 0;
    for (int i = 0; i < child_procs.n_cmds; ++i) {
      if (child_procs.cmds[i].watch)
        watch_cmds[watch_cmd_end++] = child_procs.cmds + i;
    }
  }

  install_term_and_int_handlers();

  int error_code;
  run_configure_cmds(child_procs.n_cmds, child_procs.cmds);

  if (running) {
    run_cmds(child_procs.n_cmds, child_procs.cmds);
    error_code = wait_for_requested_commands_to_exit(n_watch_cmds, watch_cmds);
    remove_term_and_int_handlers();
    alarm(WAIT_FOR_PROC_DEATH_TIMEOUT);
    kill(opts.signal_everything ? -1 : 0, SIGTERM);
    wait_for_all_processes_to_exit(error_code);

    DPRINTF("all processes exited cleanly");
  }

  free(watch_cmds);
  free(child_procs.cmds);
  return error_code;
}
