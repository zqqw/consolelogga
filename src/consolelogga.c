/*
 * consolelogga, logs console messages.
 * Copyright (c) 2017 www.cxperimental.weebly.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

/* Compile with args specified by configure script */
#include "consolelogga.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <argp.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <ctype.h>
#include <signal.h>

#define gettext_noop(String) String
#define N_(String) gettext_noop (String)
/* N_() marks string initializers for xgettext */
#ifdef CONSOLELOGGA_USE_GETTEXT
#include <libintl.h>
#include <locale.h>
#define _(String) gettext (String)
#else
#define _(String) String
#endif

#define PROC_C "/proc/consoles"
#define TTY_DIR "/dev/"
#define DEFAULT_LOGFILE "/var/log/consolelogga.log"
#define DEFAULT_PIDFILE "/run/consolelogga/consolelogga_pidfile.pid"
#define STR_SZ 2048

#define X_FINITE_FAILURE_RETRY(expression) \
  (__extension__                                                              \
    ({ long int result;                                                       \
       long int index = 0;                                                    \
       do result = (long int) (expression);                                   \
       while (result == -1L && errno == EINTR && (index++) < 1000);          \
       result; }))

void signal_setup (void);
void unset_restart ();
void reset_restart ();
void signal_unblock (void);
void * xmalloc (size_t size);
void * xrealloc (void *ptr, size_t size);
void error_msg (char *msg_str, const int err_no);
int main (int argc, char **argv);
static error_t parse_opt (int key, char *arg, struct argp_state *state);
void fork_to_background (void);
int input_loop (void);
int read_user_consoles (void);
int open_pty_pair (int *amaster, int *aslave);
int write_pidfile (void);
void get_tokens (char *orig_path, int len);
int c_mkdir_p (int len);
int read_proc_consoles (void);
void set_proc_consoles_node (char *in_str);
void output_to_consoles (ssize_t len1);
int open_output_tty (void);
void node_update (int bytes_val);

volatile sig_atomic_t fin = 0, sigsave = 0;
char *log_file = DEFAULT_LOGFILE;
char str1[STR_SZ];
char strtmp[STR_SZ];
char * timestamp = NULL;
char **path_arr = NULL;
int fork_flag = 1;
int exact_flag = 1;
char *pidfilestr = NULL;
char *consolestr = NULL;

/* Argp global variables */
const char *argp_program_version = "consolelogga 1.1.0";
const char *argp_program_bug_address = "<www.cxperimental.weebly.com>";

/* Argp program documentation */
static char doc[] =
N_("consolelogga : logs messages from the console at boot or shutdown\n\n\n\
Default values, # consolelogga (with no options) :\n\
Fork to background, no pidfile, console(s) found in /proc/consoles,\n\
log to /var/log/consolelogga.log (if it exists)\n\
Examples of use:\n\
# consolelogga -xp  :  foreground with default pidfile\n\
# consolelogga --pidpath=/new/path  :  custom pidfile location\n\
# consolelogga -o /new/path  :  custom logfile location\n\
# consolelogga -c\"/dev/tty1 /dev/tty2\"  :  write to tty1 & 2\n");
/* Argp options */
static struct argp_option options[] = {
  {"console", 'c', N_("consolearg"), 0, N_("Write to custom console(s)"), 0},
  {"nofork", 'x', 0, 0, N_("Run as a foreground process, don't fork"), 0},
  {"pidfile", 'p', 0, 0, N_("Write a pidfile to default location"), 0},
  {"exact", 'e', 0, 0, N_("Log messages without a timestamp"), 0},
  {"pidpath", 'P', N_("/path/to/pid"), 0, N_("Define pidfile location"), 0},
  {"output", 'o', N_("/path/to/log"), 0, N_("Define log file location"), 0},
  {0, 0, 0, 0, 0, 0}
};

/* Argp parser */
static struct argp argp = {options, parse_opt, NULL, doc, NULL, 0, 0};

struct line_list
{
  char *line_str;
  int line_len;
  struct line_list *next;
};

struct line_list *ll_start = NULL;
struct line_list *ll_curr = NULL;
struct line_list *ll_tmp = NULL;

struct consoles_list
{
  char *tty_name;
  int tty_fd;
  struct consoles_list *next;
};

struct consoles_list *c_list_root = NULL;
struct consoles_list *c_list_curr = NULL;

/* remember directories created for cleanup later */
struct dir_list
{
  char *dir_str;
  struct dir_list *prev;
};

struct dir_list *dl_curr = NULL;
struct dir_list *dl_tmp = NULL;

/* Signal handler functions */
void
fin_handler (int signum)
{
  fin++;
  sigsave = signum;
}

/* Setup signal handler functions for various signals */
void
signal_setup (void)
{
  struct sigaction newaction, oldaction;

  sigemptyset (&newaction.sa_mask);
  newaction.sa_flags = SA_RESTART;
  newaction.sa_handler = fin_handler;
  oldaction.sa_handler = NULL;
  sigemptyset (&oldaction.sa_mask);
  errno = 0;
  if (sigaction (SIGTERM, NULL, &oldaction)) error_msg ("get SIGTERM", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGTERM, &newaction, NULL)) error_msg ("SIGTERM", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGINT, NULL, &oldaction)) error_msg ("get SIGINT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGINT, &newaction, NULL)) error_msg ("SIGINT", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGQUIT, NULL, &oldaction)) error_msg ("get SIGQUIT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGQUIT, &newaction, NULL)) error_msg ("SIGQUIT", errno);
    }
  newaction.sa_handler = SIG_IGN; /* Ignore SIGHUP SIGUSR1 SIGUSR2 */
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGHUP, NULL, &oldaction)) error_msg ("get SIGHUP", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGHUP, &newaction, NULL)) error_msg ("SIGHUP", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGUSR1, NULL, &oldaction)) error_msg ("get SIGUSR1", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGUSR1, &newaction, NULL)) error_msg ("SIGUSR1", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGUSR2, NULL, &oldaction)) error_msg ("get SIGUSR2", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGUSR2, &newaction, NULL)) error_msg ("SIGUSR2", errno);
    }
}

void
unset_restart (void)
{
  struct sigaction newaction, oldaction;

  sigemptyset (&newaction.sa_mask);
  newaction.sa_flags = 0; /* SA_RESTART would block on read indefinitely */
  newaction.sa_handler = fin_handler;
  oldaction.sa_handler = NULL;
  sigemptyset (&oldaction.sa_mask);
  errno = 0;
  if (sigaction (SIGTERM, NULL, &oldaction)) error_msg ("get SIGTERM", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGTERM, &newaction, NULL)) error_msg ("SIGTERM", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGINT, NULL, &oldaction)) error_msg ("get SIGINT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGINT, &newaction, NULL)) error_msg ("SIGINT", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGQUIT, NULL, &oldaction)) error_msg ("get SIGQUIT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGQUIT, &newaction, NULL)) error_msg ("SIGQUIT", errno);
    }
}

void
reset_restart (void)
{
  struct sigaction newaction, oldaction;

  sigemptyset (&newaction.sa_mask);
  newaction.sa_flags = SA_RESTART;
  newaction.sa_handler = fin_handler;
  oldaction.sa_handler = NULL;
  sigemptyset (&oldaction.sa_mask);
  errno = 0;
  if (sigaction (SIGTERM, NULL, &oldaction)) error_msg ("get SIGTERM", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGTERM, &newaction, NULL)) error_msg ("SIGTERM", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGINT, NULL, &oldaction)) error_msg ("get SIGINT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGINT, &newaction, NULL)) error_msg ("SIGINT", errno);
    }
  oldaction.sa_handler = NULL;
  errno = 0;
  if (sigaction (SIGQUIT, NULL, &oldaction)) error_msg ("get SIGQUIT", errno);
  if (oldaction.sa_handler != SIG_IGN)
    {
      errno = 0;
      if (sigaction (SIGQUIT, &newaction, NULL)) error_msg ("SIGQUIT", errno);
    }
}

/* Helper function used for cleanup at exit */
void
signal_unblock (void)
{
 struct sigaction newaction;
  newaction.sa_handler = SIG_DFL;
  sigemptyset (&newaction.sa_mask);
  errno = 0;
  if (sigaction (sigsave, &newaction, NULL))
    {
      error_msg (_("sigaction failure at cleanup"), errno);
      exit (EXIT_FAILURE);
    }
}

void *
xmalloc (size_t size)
{
  errno = 0;
  void *value = malloc (size);
  if (value == 0)
    {
      error_msg (_("xmalloc virtual memory exhausted"), errno);
      exit (EXIT_FAILURE);
    }
  return value;
}

void *
xrealloc (void *ptr, size_t size)
{
  errno = 0;
  void *value = realloc (ptr, size);
  if (value == 0)
    {
      error_msg (_("xrealloc virtual memory exhausted"), errno);
      exit (EXIT_FAILURE);
    }
  return value;
}

/* checks fork_flag to see where to print messages */
void
error_msg (char *msg_str, const int err_no)
{
  FILE * log_stream;
  struct stat file_stat;

  if (!(stat (log_file, &file_stat)) &&
      (S_ISREG (file_stat.st_mode)))
    { /* file exists and is a regular file */
      errno = 0;
      log_stream = fopen (log_file, "a");
      if (log_stream == NULL)
        {
          if (fork_flag < 2) perror (_("fopen failed to open log_file"));
        }
      else
        {
          fprintf (log_stream, "consolelogga : %s : %s\n", msg_str,
                   (err_no == 0) ? "" : strerror (err_no));
          fclose (log_stream);
        }
    }
  if (fork_flag < 2)
    {
      fprintf (stderr, "consolelogga : %s : %s\n", msg_str,
               (err_no == 0) ? "" : strerror (err_no));
    }
}

int
main (int argc, char **argv)
{
  int rval;

#ifdef CONSOLELOGGA_USE_GETTEXT
  bindtextdomain ("consolelogga", USRPREFIX "/share/locale");
/* Instead of: setlocale (LC_ALL, ""); this is better
 * setlocale sets these values from the users env to use here
 * If not set in early boot do LANG=x consolelogga or default to en_US.utf8
 */
  setlocale (LC_CTYPE, "");
  setlocale (LC_MESSAGES, "");
  setlocale (LC_COLLATE, "");
  setlocale (LC_MONETARY, "");
  setlocale (LC_NUMERIC, "");
  setlocale (LC_TIME, "");
  textdomain ("consolelogga");
#endif

  argp_parse (&argp, argc, argv, 0, 0, 0); /* Parse arguments */
  if (fork_flag) fork_to_background ();
  signal_setup ();
  rval = input_loop ();
  if ((pidfilestr != NULL)  && (!rval))
    {
      errno = 0;
      if (unlink (pidfilestr))
        {
          error_msg (_("pidfile unlink failed in cleanup"), errno);
        }
      if (dl_curr != NULL) /* rmdir the formerly created empty directories */
        {
          while (1)
            {
              errno = 0;
              if (rmdir (dl_curr->dir_str))
                {
                  error_msg (_("rmdir failed in cleanup"), errno);
                  break; /* no point continuing, next dirs contain a dir */
                }
              if (dl_curr->prev == NULL) break; /* normal loop end point */
              dl_curr = dl_curr->prev;
            }
        }
    }
  if (fin) /* Unblock then raise signal for correct exit code */
    {
      signal_unblock ();
      raise (sigsave);
      error_msg (_("Fatal signal not fatal - terminating"), errno);
      exit (EXIT_FAILURE);
    }
  return (rval) ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Argp, parse a single option */
static error_t
parse_opt (int key, char *arg,
           __attribute__ ((unused)) struct argp_state *state)
{ /* Get the input argument from argp_parse */
  switch (key)
    {
    case 'c':
      consolestr = arg;
      break;
    case 'x':
      fork_flag = 0;
      break;
    case 'p':
      pidfilestr = DEFAULT_PIDFILE;
      break;
    case 'e':
      exact_flag = 0;
      break;
    case 'P':
      pidfilestr = arg;
      break;
    case 'o':
      log_file = arg;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

void
fork_to_background (void)
{
  pid_t pid, sid;

  errno = 0;
  pid = fork ();
  if (pid < 0)
    {
      perror (_("Fork failed"));
      exit (EXIT_FAILURE);
    }
  if (pid > 0) exit (EXIT_SUCCESS);
  fork_flag++;
  umask (0); /* set file permission mask to 0 */
  errno = 0;
  sid = setsid();
  if (sid < 0)
    {
      error_msg (_("setsid failed"), errno);
      exit (EXIT_FAILURE);
    }
  /* 2nd fork PID != SID, not session leader, won't get controlling tty */
  errno = 0;
  pid = fork ();
  if (pid < 0)
    {
      error_msg (_("Second fork failed"), errno);
      exit (EXIT_FAILURE);
    }
  if (pid > 0) exit (EXIT_SUCCESS);
  errno = 0;
  if ((chdir ("/")) < 0)
    {
      error_msg (_("chdir failed"), errno);
      exit (EXIT_FAILURE);
    }
  fclose (stdin);
  fclose (stdout);
  fclose (stderr);
  stdin = fopen ("/dev/null", "r");
  stdout = fopen ("/dev/null", "w");
  stderr = fopen ("/dev/null", "w");
}

/* Central daemon loop */
int
input_loop (void)
{
  int i;
  int u_len = 83;
  int ptymaster = 0;
  int ptyslave = 0;
  int create_files = 1;
  int find_consoles = 1;
  ssize_t bytes_read = 1;

  if (consolestr != NULL)
    {
      find_consoles = (read_user_consoles ()) ? 0 : 1;
    }
  ll_start = (struct line_list *) xmalloc (sizeof (struct line_list));
  ll_curr = ll_start;
  ll_curr->line_str = (char *) xmalloc (u_len + 1); /* mark transition */
  ll_curr->line_str[0] = '\n'; /* last entry might not end with a \n */
  for (i = 1; i < u_len; i++)
    {
      if (i < (u_len - 3)) ll_curr->line_str[i] = '_';
      else ll_curr->line_str[i] = '\n';
    }
  ll_curr->line_str[i] = '\0';
  ll_curr->line_len = u_len;
  ll_curr->next = (struct line_list *) xmalloc (sizeof (struct line_list));
  ll_curr = ll_curr->next;
  ll_curr->line_str = NULL;
  ll_curr->next = NULL;
  for (i = 0; i < 1000; i++)
    {
      if (open_pty_pair (&ptymaster, &ptyslave)) break;
      if (i < 1000) continue;
      error_msg (_("Can't open pty - terminal failure"), 0);
      return (1);
    } /* Get the output from /dev/console */
  errno = 0;
  if (ioctl (ptyslave, TIOCCONS, NULL))
    {
      if (errno == EBUSY)
        {
          error_msg (_("Can't get console output, "
                     "consolelogga already running?"), errno);
        }
      else
        {
          error_msg (_("Error redirecting console output to pty slave"),
                     errno);
        }
      return (1); /* existing process = errno: Device or resource busy */
    }
  while (1) /* main loop, read blocks on /dev/console fd with no output */
    {
      if (create_files)
        {
          if (!(write_pidfile ())) create_files = 0;
        } /* retry for unmounted fs */
      if (find_consoles)
        {
          if (read_proc_consoles ()) find_consoles = 0;
        }
      unset_restart (); /* read waits for input, must wake on signals */
      bytes_read = read (ptymaster, str1, (STR_SZ - 1));
      reset_restart ();
      if (fin > 0) break; /* End loop if signalled */
      str1[bytes_read] = '\0';
      output_to_consoles (bytes_read); /* write to viewing console(s) */
      node_update (bytes_read); /* write to log or save to RAM */
    } /* while (1) */
  ioctl (0, TIOCCONS, NULL); /* release /dev/console output */
  return (0);
}

/* Get info from user argument and store entries in list node. */
/* Returns number of consoles. */
int
read_user_consoles (void)
{
  char *dev_str = NULL;
  int len = 0;
  int nlen = 0;
  int i;
  int j;
  int n = 0;
  int prefix = 1;
  int count = 0;

  len = (((int) strlen (consolestr)) + 1);
  dev_str = (char *) xmalloc (len);
  for (i = 0, j = 0; i < len; i++)
    {
      if ((isspace (consolestr[i])) || (consolestr[i] == '\0'))
        {
          if (prefix) continue; /* jump leading whitespace */
          if (c_list_root == NULL)
            { /* this is the first item, set c_list_root pointer */
              c_list_curr = (struct consoles_list *) xmalloc
                             (sizeof (struct consoles_list));
              c_list_curr->next = NULL;
              c_list_root = c_list_curr;
            }
          else
            {
              c_list_curr->next = (struct consoles_list *) xmalloc
                                   (sizeof (struct consoles_list));
              c_list_curr = c_list_curr->next;
              c_list_curr->next = NULL;
            }
          dev_str[j] = '\0';
          nlen = ((strlen (dev_str)) + 1);
          c_list_curr->tty_name = (char *) xmalloc (nlen);
          for (j = 0; n < nlen; n++, j++)
            { /* write temp str to node entry */
              c_list_curr->tty_name[n] = dev_str[j];
            }
          c_list_curr->tty_name[n] = '\0';
          c_list_curr->tty_fd = -1; /* indicates fd is closed */
          prefix = 1;
          j = 0;
          n = 0;
          count++;
          if (consolestr[i] == '\0') break;
          continue; /* more to read from the string */
        }
      dev_str[j] = consolestr[i];
      j++;
      if (prefix) prefix = 0;
    } /* get text chars until next whitespace is reached */
  return (count);
}

int
open_pty_pair (int *amaster, int *aslave)
{
  int master;
  int slave;
  char *name;

  master = getpt ();
  if (master < 0) return (0);
  if (grantpt (master) < 0 || unlockpt (master) < 0)
    {
      close (master);
      return (0);
    }
  name = ptsname (master);
  if (name == NULL)
    {
      close (master);
      return (0);
    }
  slave = open (name, O_RDWR);
  if (slave == -1)
    {
      close (master);
      return (0);
    }
  *amaster = master;
  *aslave = slave;
  return (1);
}

/* Returns 1 on error, 0 for success. */
int
write_pidfile (void)
{
  int len = 0;
  FILE * p_stream;
  pid_t current_pid;

  if (pidfilestr != NULL)
    {
      len = (int) (strlen (pidfilestr));
      if (len)
        {
          get_tokens (pidfilestr, len);
          if (c_mkdir_p (len)) return (1);
          p_stream = fopen (pidfilestr, "w"); /* open & truncate to 0 length */
          if (p_stream == NULL)
            {
              error_msg (_("fopen failed to open pid file"), 0);
              return (1);
            }
          current_pid = getpid ();
          fprintf (p_stream, "%d\n", (int) current_pid);
          fclose (p_stream);
        }
    }
  return (0);
}

/* Breaks down file path string into individual elements, no return value. */
void
get_tokens (char *orig_path, int len)
{
  char *token;
  const char dl[] = "/";
  char *path_copy;
  char *free_ptr;
  char *unit = NULL;
  int count = 0;

  path_copy = (char *) xmalloc (len + 1);
  free_ptr = path_copy;
  strcpy (path_copy, orig_path);
  /* Write tokens to array with unknown no. of strings of unknown length */
  do
    {
      token = strtok (path_copy, dl);
      path_copy = NULL; /* Set to NULL for strtok after first loop */
      path_arr = (char **) xrealloc (path_arr,
                                    ((count + 1) * (sizeof (char *))));
      if (token != NULL)
        { /* strlen on NULL token gives run time segmentation fault */
          unit = (char *) xmalloc ((strlen (token)) + 1);
          strcpy (unit, token);
          path_arr[count] = unit;
          unit = NULL;
        }
      else
        {
          path_arr[count] = NULL;
        }
      count++;
    }
  while (token != NULL); /* No problem in c_mkdir_p if zero tokens found. */
  free (free_ptr);
}

/* mkdir -p in C, returns 1 on failure, 0 for success. */
int
c_mkdir_p (int len)
{
  struct stat test_stat;
  char *temp_str;
  int i;
  int j = 0;
  int k;
  int l;
  int token_len;

  errno = 0;
  if (!(stat ("/", &test_stat)))
    { /* root exists */
      if (!(S_ISDIR (test_stat.st_mode)))
        { /* root isn't a directory */
          error_msg (_("Root isn't a directory"), errno);
          return (1);
        }
    }
  else
    { /* root doesn't exist */
      return (1);
    }
  temp_str = (char *) xmalloc (len + 1);
  temp_str[j] = '/'; /* write root to str */
  temp_str[++j] = '\0'; /* first increment j then null terminate str */
  for (i = 0; path_arr[i + 1] != NULL; i++)
    {
      k = 0;
      token_len = (int) strlen (path_arr[i]);
      while (k < token_len)
        {
          temp_str[j] = path_arr[i][k];
          j++;
          k++;
        }
      temp_str[j+1] = '\0';
      errno = 0;
      if (!(stat (temp_str, &test_stat))) /* does it exist ? */
        {
          if (!(S_ISDIR (test_stat.st_mode))) /* not a directory ? */
            {
              error_msg (_("Path includes non-directory element"), 0);
              return (1);
            }
        }
      else
        { /* make the latest directory in the path */
          errno = 0;
          if (mkdir (temp_str, S_IRWXU | S_IRGRP | S_IXGRP
              | S_IROTH | S_IXOTH))
            {
              error_msg (_("mkdir - failed to create directory"), errno);
              return (1);
            }
          if (dl_curr == NULL) /* store created dir(s) for cleanup */
            { /* this is the first line found */
              dl_curr = (struct dir_list *) xmalloc
                         (sizeof (struct dir_list));
              dl_curr->dir_str = (char *) xmalloc (j + 1);
              for (l = 0; l <= j; l++)
                {
                  dl_curr->dir_str[l] = temp_str[l];
                }
              dl_curr->prev = NULL;
            }
          else
            {
              dl_tmp = (struct dir_list *) xmalloc
                        (sizeof (struct dir_list));
              dl_tmp->dir_str = (char *) xmalloc (j + 1);
              for (l = 0; l <= j; l++)
                {
                  dl_tmp->dir_str[l] = temp_str[l];
                }
              dl_tmp->prev = dl_curr;
              dl_curr = dl_tmp;
              dl_tmp = NULL;
            }
        }
      temp_str[j] = '/';
      temp_str[++j] = '\0';
    }
  for (i = 0; path_arr[i] != NULL; i++)
    {
      if (path_arr[i] != NULL)
        {
          free (path_arr[i]);
          path_arr[i] = NULL;
        }
    }
  if (path_arr != NULL)
    {
      free (path_arr);
      path_arr = NULL;
    }
  if (temp_str != NULL)
    {
      free (temp_str);
      temp_str = NULL;
    }
  return (0);
}

/* Not called if user defined consoles, returns 0 for failure. */
int
read_proc_consoles (void)
{
  char *cstr = NULL;
  ssize_t get_ret = (ssize_t) 0;
  size_t n_bytes = (size_t) 0;
  FILE *stream1;
  int count = 0;

  errno = 0;
  stream1 = fopen (PROC_C, "r");
  if (stream1 == NULL)
    {
      error_msg (_("fopen failed to open /proc/consoles"), errno);
      return (0);
    }
  while (get_ret != -1)
    { /* loop & read lines from file */
      get_ret = getline (&cstr, &n_bytes, stream1);
      if (get_ret > 1) /* ignore empty lines */
        {
          count++;
          if (c_list_root == NULL)
            { /* this is the first line found, set c_list_root pointer */
              c_list_curr = (struct consoles_list *) xmalloc
                             (sizeof (struct consoles_list));
              c_list_curr->next = NULL;
              c_list_root = c_list_curr;
            }
          else
            {
              c_list_curr->next = (struct consoles_list *) xmalloc
                                   (sizeof (struct consoles_list));
              c_list_curr = c_list_curr->next;
              c_list_curr->next = NULL;
            }
          set_proc_consoles_node (cstr);
        }
    }
  if (!count) error_msg (_("No consoles found in /proc/consoles"), 0);
  return (count); /* number of consoles found */
}

/* Get info from string and store in linked list node */
void
set_proc_consoles_node (char *in_str)
{
  char *dev_str = NULL;
  int len = 0;
  int dlen = 0;
  int i;
  int j;
  int prefix = 1;

  len = (((int) strlen (in_str)) + 1);
  dev_str = (char *) xmalloc (len);
  for (i = 0, j = 0; i < len; i++)
    {
      if (in_str[i] == '\0')
        {
          break;
        }
      if (isspace (in_str[i]))
        {
          if (prefix) continue; /* go past leading whitespace */
          break; /* reached trailing whitespace */
        }
      dev_str[j] = in_str[i];
      j++;
      if (prefix) prefix = 0;
    } /* get text chars up to next whitespace */
  dev_str[j] = '\0';
  if (j == 4 && dev_str[0] == 't' && dev_str[1] == 't' && dev_str[2] == 'y' &&
      dev_str[3] == '0')
    { /* console device is tty0, output device should be tty1 */
      dev_str[3] = '1';
    }
  dlen = strlen (TTY_DIR);
  len = ((strlen (dev_str)) + (dlen + 1));
  c_list_curr->tty_name = (char *) xmalloc (len);
  for (i = 0, j = 0; i < dlen; i++, j++)
    {
      c_list_curr->tty_name[i] = TTY_DIR[j];
    }
  for (j = 0; i < (len); i++, j++)
    {
      c_list_curr->tty_name[i] = dev_str[j];
    }
  c_list_curr->tty_name[i] = '\0';
  c_list_curr->tty_fd = -1; /* indicates fd is closed */
}

/* Consoles list is filled, open fd if needed, write output to each entry.
 * Close fd and abandon this line for most errors, try again next line.
 */
void
output_to_consoles (ssize_t len1)
{
  ssize_t nbytes = 0;
  ssize_t bytes_total = 0;
  int i;

  c_list_curr = c_list_root;
  while (c_list_curr != NULL)
    {
      if (c_list_curr->tty_fd == -1)
        {
          c_list_curr->tty_fd = open_output_tty ();
        }
      if (c_list_curr->tty_fd != -1)
        { /* write str1 to output tty, error check */
          for (i = 0; i < 1000; i++)
            { /* don't loop forever, non-essential for boot sequence */
              errno = 0;
              nbytes = write (c_list_curr->tty_fd, str1, len1);
              if (nbytes == -1)
                {
                  if ((errno == EAGAIN) || (errno == EINTR))
                    {
                      continue;
                    }
                  else
                    {
                      close (c_list_curr->tty_fd);
                      c_list_curr->tty_fd = -1;
                      break;
                    }
                }
              bytes_total += nbytes;
              if (bytes_total >= len1) break;
            }
        }
      c_list_curr = c_list_curr->next;
    }
}

int
open_output_tty (void)
{
  int oldflags = 0;
  int rval;
  int fd;

  errno = 0;
  fd = open (c_list_curr->tty_name, O_WRONLY | O_NONBLOCK | O_NOCTTY);
  if (fd == -1)
    {
      error_msg (_("Unable to open output tty"), errno);
      return (-1);
    }
  errno = 0;
  oldflags = fcntl (fd, F_GETFL, 0);
  if (oldflags == -1)
    {
      close (fd);
      error_msg (_("fcntl F_GETFL failed"), errno);
      return (-1);
    }
  oldflags &= ~O_NONBLOCK;
  errno = 0;
  rval = fcntl (fd, F_SETFL, oldflags);
  if (rval == -1)
    {
      close (fd);
      error_msg (_("fcntl F_SETFL failed"), 0);
      return (-1);
    }
  return (fd);
}

/* Add time and date to console entries if required, save new line in a node.
 * Write any pending nodes to the log if possible and update linked list.
 */
void
node_update (int bytes_val)
{
  time_t time_simple = (time_t) 0;
  struct tm * time_broken = NULL;
  size_t nchars = (size_t) 0;
  int i = 0;
  int j;
  int k;
  struct stat file_stat;
  int log_fd;
  ssize_t nbytes = 0;
  ssize_t bytes_total = 0;

  if (exact_flag)
    {
      time_simple = time (NULL);
      if (time_simple == (time_t) -1)
        {
          error_msg (_("time returned -1"), 0);
        }
      time_broken = localtime (&time_simple);
      if (time_broken == NULL)
        {
          error_msg (_("localtime returned NULL"), 0);
        }
      nchars = strftime (NULL, (size_t) 256, "%x %X : ", time_broken);
      /* realloc does nothing if size is unchanged, which it usually is */
      timestamp = (char *) xrealloc (timestamp, nchars + (size_t) 1);
      nchars = strftime (timestamp, (size_t) 256, "%x %X : ", time_broken);
    }
  ll_curr->line_len = (bytes_val + nchars);
  ll_curr->line_str = (char *) xmalloc (ll_curr->line_len + 1);
  if (exact_flag)
    {
      for (i = 0; i < (int) nchars; i++)
        {
          ll_curr->line_str[i] = timestamp[i];
        }
    }
  for (j = 0; j < bytes_val; i++, j++)
    {
      ll_curr->line_str[i] = str1[j];
    }
  ll_curr->line_str[i] = '\0';
  ll_curr->next = (struct line_list *) xmalloc (sizeof (struct line_list));
  ll_curr = ll_curr->next;
  ll_curr->line_str = NULL;
  ll_curr->next = NULL;
  /* write any entries saved in the linked list to the log file */
  while (ll_start->next != NULL)
    { /* check if log is writable as entries are written */
      if (!(stat (log_file, &file_stat)) &&
          (S_ISREG (file_stat.st_mode)))
        { /* file exists and is a regular file */
          log_fd = open (log_file, O_NONBLOCK | O_APPEND | O_WRONLY);
          if (log_fd == -1)
            { /* open failed */
              break;
            }
        }
      else
        {
          break;
        }
      for (k = 0; k < 1000; k++)
        {
          errno = 0;
          nbytes = write (log_fd, ll_start->line_str, ll_start->line_len);
          if (nbytes == -1)
            {
              if ((errno == EAGAIN) || (errno == EINTR))
                {
                  continue;
                }
              else
                {
                  close (log_fd);
                  log_fd = -1;
                  break;
                }
            }
          bytes_total += nbytes;
          if (bytes_total >= ll_start->line_len) break;
        }

      ll_tmp = ll_start;
      ll_start = ll_tmp->next;
      free (ll_tmp->line_str);
      free (ll_tmp);
      ll_tmp = NULL;
      close (log_fd);
    }
  str1[0] = '\0';
}
