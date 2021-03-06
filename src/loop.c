/*
** Copyright (C) 2002-2008 Christophe Kalt
**
** This file is part of shmux,
** see the LICENSE file for details on your rights.
*/

#include "os.h"

#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>		/* FreeBSD wants this for the next one.. */
#include <sys/resource.h>
#include <poll.h>
#include <time.h>

#if !defined(WCOREDUMP)
# define WCOREDUMP(x) 0
#endif

#include "analyzer.h"
#include "byteset.h"
#include "exec.h"
#include "loop.h"
#include "siglist.h"
#include "status.h"
#include "target.h"
#include "term.h"

static char const rcsid[] = "@(#)$Id$";

extern char *myname;

struct child
{
    pid_t	pid;		/* Process ID */
    int		num;		/* target number */
    int		test, passed;	/* test?, passed? */
    int		analyzer;	/* analyzer? */
    int		output;		/* output mode */
    int		execstate;	/* exec() status: 0=ok, 1=failed? 2=failed */
    time_t	timeout;	/* timeout expiration time */
    int		timedout;	/* 0=no, 1=SIGTERM sent, 2=SIGKILL sent */
    char	*obuf, *ebuf;	/* stdout/stderr truncated buffer */
    char	*ofname, *efname; /* stdout/stderr file names */
    int		ofile, efile;	/* stdout/stderr file fd */
    int		status;		/* waitpid(status) */
    time_t	orphan;		/* orphan debug message rate limit */
};

static int got_sigint;
#define SPAWN_FATAL 0
#define SPAWN_ABORT 1
#define SPAWN_QUIT  2
#define SPAWN_PAUSE 3
#define SPAWN_CHECK 4
#define SPAWN_NONE  5
#define SPAWN_ONE   6
#define SPAWN_MORE  7
static int spawn_mode;
static int failure_mode = SPAWN_MORE; /* Historical default */

static void shmux_sigint(int);
static void setup_fdlimit(int, int);
static void init_child(struct child *);
static void parse_child(char *, int, int, int, struct child *, int, char *);
static void parse_fping(char *);
static void parse_user(int, struct child *, int);
static int  output_file(char **, char *, char *, char *);
static void output_show(char *, int, char *, int);
static void set_cmdstatus(int);

/*
** shmux_sigint
**	SIGINT handler
*/
static void
shmux_sigint(sig)
int sig;
{
  got_sigint += 1;
}

/*
** setup_fdlimit
**	Since there's a limit on the number of open file descriptors a
**	process may have, we do some math and try to avoid running into
**	it which could be unpleasant depending on what we're trying to
**	achieve when we run out.
*/
void
setup_fdlimit(fdfactor, max)
int fdfactor, max;
{
    struct rlimit fdlimit;

    /*
    ** The assumptions are:
    ** + 3 for stdin, stdout and stderr (our own)
    ** + 3 for stdin, stdout and stderr for fping
    ** + 3 for pipe creation in exec.c/exec()
    ** + (3 or 5 * max) for children stdin, stdout and stderr
    ** And we add another 10 as safety margin (2 /dev/tty, and "unknowns")
    */
    if (getrlimit(RLIMIT_NOFILE, &fdlimit) == -1)
      {
	eprint("getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
	exit(RC_ERROR);
      }
    if (fdlimit.rlim_cur < (max + 3) * fdfactor + 10)
      {
	fdlimit.rlim_cur = (max + 3) * fdfactor + 10;
	if (fdlimit.rlim_cur > fdlimit.rlim_max)
	    fdlimit.rlim_cur = fdlimit.rlim_max;

	if (setrlimit(RLIMIT_NOFILE, &fdlimit) == -1)
	    eprint("setrlimit(RLIMIT_NOFILE, %d): %s",
		   (int) fdlimit.rlim_cur, strerror(errno));

	if (getrlimit(RLIMIT_NOFILE, &fdlimit) == -1)
	  {
	    eprint("getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
	    eprint("Unable to validate parallelism factor.");
	  }
	else if (fdlimit.rlim_cur < (max + 3) * fdfactor + 10)
	  {
	    int old;

	    old = max;
	    max = ((fdlimit.rlim_cur - 10) / fdfactor) - 3;
	    eprint("Reducing parallelism factor to %d (from %d) because of system limitation.", max, old);
	  }
      }
	
#if defined(__NetBSD__)
    /* See NetBSD PR#17507 */
    {
      int i, *fds;
      
      fds = (int *) malloc(fdlimit.rlim_cur * sizeof(int));
      if (fds == NULL)
	{
	  perror("malloc failed");
	  exit(RC_ERROR);
	}
      i = -1;
      do
	  fds[++i] = dup(0);
      while (i < fdlimit.rlim_cur && fds[i] != -1);
      dprint("Duped %d fds to get around NetBSD's broken poll(2)", i);
      while (i >= 0)
	  close(fds[i--]);
      free(fds);
    }
#endif
}

/*
** init_child
**	Used to initialize a child structure whenever a new child is spawned.
*/
static void
init_child(kid)
struct child *kid;
{
    kid->num = target_getnum();
    kid->test = kid->passed = 0;
    kid->analyzer = 0;
    kid->output = OUT_MIXED;
    kid->execstate = 0;
    kid->timeout = 0;
    kid->timedout = 0;
    kid->obuf = kid->ebuf = NULL;
    kid->ofname = kid->efname = NULL;
    kid->ofile = kid->efile = -1;
    kid->status = -1;
    kid->orphan = 0;

    status_spawned(1);
}

/*
** parse_child
**	Parse output from children
*/
static void
parse_child(name, isfping, verbose_tests, analyzer, kid, std, buffer)
char *name, *buffer;
int isfping, verbose_tests, analyzer, std;
struct child *kid;
{
    char *start, *nl;

    assert( std == 1 || std == 2 );

    start = nl = buffer;

    while (*nl != '\0')
      {
	char **left;

	if (*nl != '\n') /* Purify reports *1* UMR here?!? */
	  {
	    nl += 1;
	    continue;
	  }

	/* Got an end of line, trim \r\n  */
	if (*(nl-1) == '\r') /* XXX */
	    *(nl-1) = '\0';
	else
	    *nl = '\0';

	left = NULL;
	/* Check which state the child is in. */
	switch (kid->execstate)
	  {
	  case 2:
	      /* Error from exec() */
	      eprint("Fatal error for %s: %s", name, start);
	      break;
	  case 1:
	      /* Possible error message from exec() */
	      if (strcmp(start, "SHMUCK!") == 0)
		{
		  /* Should be from exec() */
		  kid->execstate = 2;
		  break;
		}
	      /* Can't be.. */
	      eprint("Unexpected meaningless SIGTSTP received by child spawned for '%s'.  Recovering..", name);
	      kid->execstate = 0;
	      /* FALLTHROUGH */
	  case 0:
	      /* General case, we read output from child */
	      if (std == 1)
		  left = &(kid->obuf); /* stdout */
	      else
		  left = &(kid->ebuf); /* stderr */
	      
	      if (isfping == 0)
		{
		  /* Either a test or real command */
		  if (kid->test == 1)
		    {
		      /*
		      ** `SHMUX.' must arrive all at once (including the \n).
		      ** Considered reasonnable until proven to be a bug.
		      */
		      if (strcmp(start, "SHMUX.") == 0
			  && kid->passed == 0 && std == 1)
			  kid->passed = 1;
		      else
			  kid->passed = -1;

		      if (verbose_tests == 1 && kid->passed == -1)
			  eprint("Test output for %s: %s%s",
				 name, (*left == NULL) ? "" : *left, start);
		      else
			  dprint("Test output for %s: %s%s",
				 name, (*left == NULL) ? "" : *left, start);
		    }
		  else
		    {
		      if ((kid->output & OUT_ERR) == 0
			  && (analyzer == ANALYZE_LNRE
			      || analyzer == ANALYZE_LNPCRE))
			{
			  /* Line based analyzer is used, get to work */
			  char *str;

			  if (*left == NULL)
			      str = start;
			  else
			    {
			      str = (char *) malloc(strlen(*left)
						    + strlen(start) + 1);
			      snprintf(str, strlen(*left) + strlen(start) + 1,
					"%s%s", *left, start);
			    }

			  if (analyzer_lnrun(analyzer,
					     (std == 1) ? ANALYZE_STDOUT
					     : ANALYZE_STDERR, str) != 0)
			    {
			      if ((kid->output & OUT_IFERR) != 0
				  && (kid->output & OUT_MIXED) != 0)
				  {
				    assert( (kid->output & OUT_COPY) != 0 );
				    output_show(name, kid->ofile, kid->ofname,
						std);
				    output_show(name, kid->efile, kid->efname,
						std);
				  }
			      kid->output &= ~OUT_IFERR;
			      kid->output |= OUT_ERR;
			      eprint("Analysis of %s output indicates an error", name);
			    }
			  if (*left != NULL)
			      free(str);
			}
		      if ((kid->output & OUT_MIXED) != 0
			  && (kid->output & OUT_IFERR) == 0)
			  /* Outputing to screen */
			  tprint(name, ((std == 1) ? MSG_STDOUT : MSG_STDERR),
				 "%s%s", (*left == NULL) ? "" : *left, start);
		      if (kid->ofile != -1)
			{
			  /* Outputing to a file, so need to add \r\n back */
			  if (*(nl-1) == '\0') /* XXX */
			      *(nl-1) = '\r';
			  if ((left != NULL && *left != NULL &&
			       write((std == 1) ? kid->ofile : kid->efile,
				     *left, strlen(*left)) == -1)
			      ||
			      write((std == 1) ? kid->ofile : kid->efile,
				    start, strlen(start)) == -1
			      ||
			      write((std == 1) ? kid->ofile : kid->efile,
				    "\n", 1) == -1)
			      /* Should we do a little more here? */
			      eprint("Data lost for %s, write() failed: %s",
				     name, strerror(errno));
			}
		    }
		}
	      else
		  parse_fping(start);

	      break;
	  default:
	      abort();
	  }
	
	start = nl += 1;
	if (left != NULL && *left != NULL)
	  {
	    free(*left);
	    *left = NULL;
	  }
      }

    if (start != nl)
      {
	/* There is some leftover data not terminated by \n */
	assert( start < nl );
	if (isfping == 0)
	  {
	    char **left;

	    if (std == 1)
		left = &(kid->obuf); /* stdout */
	    else
		left = &(kid->ebuf); /* stderr */

	    if (*left == NULL)
		*left = strdup(start);
	    else
	      {
		int leftlen;
		leftlen = strlen(*left);
		
		if (leftlen > 1024)
		  {
		    if (kid->ofile != -1)
		      {
			if (write((std == 1) ? kid->ofile : kid->efile,
				  *left, leftlen) == -1)
			    /* Should we do a little more here? */
			    eprint("Data lost for %s, write() failed: %s",
				   name, strerror(errno));
		      }
		    if ((kid->output & OUT_IFERR) != 0
			&& (analyzer == ANALYZE_LNRE
			    || analyzer == ANALYZE_LNPCRE))
			{
			  /*
			  ** These analyzers can't handle truncated lines,
			  ** so treat as an error.
			  */
			  if ((kid->output & OUT_MIXED) != 0)
			    {
			      output_show(name, kid->ofile, kid->ofname, std);
			      output_show(name, kid->efile, kid->efname, std);
			    }
			  kid->output &= ~OUT_IFERR;
			  kid->output |= OUT_ERR;
			  eprint("Truncated line caused analyzer failure for %s", name);
			}
		    if ((kid->output & OUT_MIXED) != 0
			&& (kid->output & OUT_IFERR) == 0)
			/* Outputing to screen */
			tprint(name,
			       ((std == 1) ? MSG_STDOUTTRUNC : MSG_STDERRTRUNC),
			       "%s", *left);
		    free(*left);
		    *left = strdup(start);
		  }
		else
		  {
		    char *old;
		    
		    old = *left;
		    *left = (char *) malloc(strlen(start) + leftlen + 1);
		    strlcpy(*left, old, strlen(start) + leftlen + 1);
		    free(old);
		    strlcpy((*left) + leftlen, start,
			sizeof((*left) + leftlen));
		  }
	      }
	  }
      	else
	  {
	    assert( isfping == 1 );
	    eprint("Truncated output from fping lost: %s", start);
	  }
      }
}

/*
** parse_fping
**	Parse one line of output from fping
*/
static void
parse_fping(line)
char *line;
{
    char *space;
		  
    space = strchr(line, ' ');
    if (space != NULL)
      {
	*space = '\0';
	if (target_pong(line) != 0 && target_setbyhname(line) != 0)
	  {
	    *space = ' ';
	    dprint("fping garbage follows:");
	    eprint("%s", line);
	  }
	else
	  {
	    *space = ' ';
	    if (strcmp(space+1, "is alive") == 0)
	      {
		iprint("%s", line);
		target_result(1);
	      }
	    else
	      {
		/* Assuming too much? */
		eprint("%s", line);
		target_result(0);
	      }
	  }
      }
    else if (strcmp(line, "") != 0)
      {
	dprint("fping garbage follows:");
	eprint("%s", line);
      }
}

/*
** parse_user
**	Handle user input
*/
static void
parse_user(c, children, max)
int c, max;
struct child *children;
{
    char *cmd;

    dprint("Current spawn mode: %d", spawn_mode);
    switch (c)
      {
      case 'h':
      case '?':
	  uprint("Available commands:");
	  uprint("      q - Quit gracefully");
	  uprint("      Q - Quit immediately");
	  uprint("<space> - Pause (e.g. Do not spawn any more children)");
	  uprint("      1 - Spawn one command, and pause if unsuccessful");
	  uprint("<enter> - Keep spawning commands until one fails");
	  uprint("      + - Always spawn more commands, even if some fail");
	  uprint("      F - Toggle failure mode to \"%s\"",
                 (failure_mode == SPAWN_PAUSE) ? "quit" : "pause");
	  uprint("      S - Show current spawn strategy");
	  uprint("      p - Show pending targets");
	  uprint("      r - Show running targets");
	  uprint("      f - Show failed targets");
	  uprint("      e - Show targets with errors");
	  uprint("      s - Show successful targets");
	  uprint("      a - Show status of all targets");
	  uprint("      k - Kill a target");
	  break;
      case 27: /* escape */
      case 'q':
	  spawn_mode = SPAWN_QUIT;
	  if (spawn_mode != SPAWN_QUIT)
	      uprint("Waiting for existing children to terminate..");
	  break;
      case 'Q':
	  spawn_mode = SPAWN_ABORT;
	  break;
      case ' ':
	  if (spawn_mode != SPAWN_PAUSE)
	      uprint("Pausing...");
	  spawn_mode = SPAWN_PAUSE;
	  break;
      case '1':
	  if (spawn_mode != SPAWN_ONE) {
              if (failure_mode == SPAWN_PAUSE)
                  uprint("Will spawn one command... (And pause on error)");
              else
                  uprint("Will spawn one command... (And quit on error)");
    }
	  if (spawn_mode != SPAWN_NONE)
	      spawn_mode = SPAWN_ONE;
	  break;
      case '\n':
      case '-':
	  if (spawn_mode != SPAWN_CHECK) {
              if (failure_mode == SPAWN_PAUSE)
                  uprint("Resuming... (Will pause on error)");
              else
                  uprint("Resuming... (Will quit on error)");
    }
	  spawn_mode = SPAWN_CHECK;
	  break;
      case '+':
	  if (spawn_mode != SPAWN_MORE)
	      uprint("Will keep spawning commands... (Even if some fail)");
	      spawn_mode = SPAWN_MORE;
	  break;
      case 'F':
          if (failure_mode == SPAWN_PAUSE)
            {
              uprint("Failure mode is now \"quit\"");
              failure_mode = SPAWN_QUIT;
            }
          else
            {
              uprint("Failure mode is now \"pause\"");
              failure_mode = SPAWN_PAUSE;
            }
          break;
      case 'S':
	  if (spawn_mode == SPAWN_QUIT)
	      uprint("Will quit once current children complete...");
	  else if (spawn_mode == SPAWN_PAUSE)
	      uprint("Paused");
	  else if (spawn_mode == SPAWN_CHECK)
              if (failure_mode == SPAWN_PAUSE)
                  uprint("Will pause if a target fails...");
              else
                  uprint("Will gracefully quit if a target fails...");
	  else if (spawn_mode == SPAWN_NONE || spawn_mode == SPAWN_ONE)
	      uprint("Will spawn only one target until it succeeds...");
	  else if (spawn_mode == SPAWN_MORE)
	      uprint("Spawning as fast as possible...");
	  else
	      uprint("Uh-oh, i don't seem to know what i'm doing! [%d]",
		     spawn_mode);
	  break;
      case 'p':
	  target_status(STATUS_PENDING);
	  break;
      case 'r':
	  target_status(STATUS_ACTIVE);
	  break;
      case 'f':
	  target_status(STATUS_FAILED);
	  break;
      case 'e':
	  target_status(STATUS_ERROR);
	  break;
      case 's':
	  target_status(STATUS_SUCCESS);
	  break;
      case 'a':
	  target_status(STATUS_ALL);
	  break;
      case 'k':
	  cmd = uprompt("kill");
	  if (cmd != NULL && cmd[0] != '\0')
	    {
	      int sig, i;
	      char *target;

	      dprint("User said to kill \"%s\"", cmd);
	      if (cmd[0] == '-')
		{
		  target = strchr(cmd, ' ');
		  if (target == NULL || target[1] == '\0')
		    {
		      uprint("No target specified.");
		      break;
		    }
		  *target = '\0';
		  target += 1;
		  if (isdigit((int) cmd[1]))
		      sig = atoi(cmd+1);
		  else
		      sig = getsignumbyname(cmd+1);
		  if (sig < 0)
		    {
		      uprint("Invalid signal name: %s", cmd);
		      break;
		    }
		}
	      else
		{
		  sig = SIGTERM;
		  target = cmd;
		}

	      if (isdigit((int) target[0]))
		{
		  if (target_setbynum(atoi(target)) != 0)
		    {
		      uprint("Invalid target number: %d", atoi(target));
		      break;
		    }
		}
	      else
		{
		  if (target_setbyname(target) != 0)
		    {
		      uprint("Invalid target: %s", target);
		      break;
		    }
		}

	      /* Find the associated child, if any! */
	      i = 0;
	      while (i <= max
		     && (children[i].pid <= 0
			 || children[i].num != target_getnum()))
		  i += 1;

	      if (i > max)
		  uprint("Target %s has no active process.", target_getname());
	      else
		{
		  if (kill(-children[i].pid, sig) != 0)
		      uprint("kill(%s, %d): %s", target_getname(), sig,
			     strerror(errno));
		  else
		      uprint("Sent signal %d to %s...", sig, target_getname());
		}
	    }
	  break;
      case 'v':
          c = term_togglemsg();
          uprint("Internal messages: %s", (c) ? "on" : "off");
          break;
      case 'D':
          c = term_toggledbg();
          uprint("Debug messages: %s", (c) ? "on" : "off");
          break;
      default:
	  uprint("Invalid Command");
	  dprint("User input = %d", c);
	  break;
      }
}

/*
** output_file
**	Create an output file.
*/
int
output_file(fname, dir, name, extension)
char **fname, *dir, *name, *extension;
{
    int sz, fd;

    assert( dir != NULL );
    assert( name != NULL );
    assert( extension != NULL );

    *fname = (char *) malloc(PATH_MAX);
    if (*fname == NULL)
      {
	perror("malloc failed");
	exit(RC_FATAL);
      }

    sz = snprintf(*fname, PATH_MAX, "%s/%s.%s", dir, name, extension);
    if (sz >= PATH_MAX)
      {
	eprint("\"%s\": name is too long", dir);
	free(*fname); *fname = NULL;
	return -1;
      }

    fd = open(*fname, O_RDWR|O_CREAT|O_EXCL, 0666);
    if (fd == -1)
      {
	eprint("open(%s): %s", *fname, strerror(errno));
	free(*fname); *fname = NULL;
	return -1;
      }

    return fd;
}

/*
** output_show
**	Show an output file.
*/
void
output_show(name, fd, fname, type)
char *name, *fname;
int fd, type;
{
    FILE *f;
    int fd2, cont;
    char buffer[8192], *nl;

    fd2 = dup(fd);
    if (fd2 < 0)
      {
	eprint("dup(%s): %s", fname, strerror(errno));
	return;
      }

    if (lseek(fd2, 0, SEEK_SET) != 0)
      {
	eprint("lseek(%s, SEEK_SET): %s", fname, strerror(errno));
	return;
      }

    f = fdopen(fd2, "r");
    if (f == NULL)
	eprint("fdopen(%s): %s", fname, strerror(errno));
    
    cont = 0;
    while (fgets(buffer, 8192, f) != NULL)
      {
	nl = strchr(buffer, '\n');
	if (nl != NULL)
	    *nl = '\0';

	if (cont == 0)
	    tprint(name, (type == 1) ? MSG_STDOUT : MSG_STDERR, "%s", buffer);
	else
	    tprint(name, (type == 1) ? MSG_STDOUTTRUNC : MSG_STDERRTRUNC,
		   "%s", buffer);

	cont = (nl == NULL) ? 1 : 0;
      }

    if (feof(f) == 0)
	eprint("fgets(%s): %s", fname, strerror(errno));

    if (fclose(f) != 0)
	eprint("fclose(%s): %s", fname, strerror(errno));
}

/*
** set_cmdstatus
**	Use to define whether a command was successful or not.
*/
static void
set_cmdstatus(result)
int result;
{
    assert( spawn_mode != SPAWN_ONE );

    if (result == CMD_SUCCESS)
      {
	if (spawn_mode == SPAWN_NONE)
	    spawn_mode = SPAWN_CHECK;
      }
    else
      {
	if (spawn_mode == SPAWN_NONE || spawn_mode == SPAWN_CHECK)
	    spawn_mode = failure_mode;
      }
    target_cmdstatus(result);
}


/*
** loop
**	Main loop.  Takes care of (optionally) pinging targets, testing
**	targets with a simple echo command, and finally running a command.
*/
int
loop(cmd, ctimeout, max, spawn, fail, outmode, odir, utest, ping, test)
char *cmd, *spawn, *ping, *odir;
int max, fail, outmode, test;
u_int ctimeout, utest;
{
    struct child *children;
    struct pollfd *pfd;
    struct sigaction sa, saved_sa;
    int idx;
    char *cargv[10];

    /* check spawn */
    if (strcmp(spawn, "all") == 0)
	spawn_mode = SPAWN_MORE;
    else if (strcmp(spawn, "check") == 0)
	spawn_mode = SPAWN_CHECK;
    else if (strcmp(spawn, "one") == 0)
	spawn_mode = SPAWN_ONE;
    else
      {
	fprintf(stderr, "%s: Invalid spawn strategy \"%s\"\n", myname, spawn);
	return RC_ERROR;
      }
    if ((spawn_mode == SPAWN_ONE || spawn_mode == SPAWN_CHECK)
        && tty_fd() < 0 && fail == 0)
        spawn_mode = SPAWN_MORE;
    if (fail == 0)
        failure_mode = SPAWN_PAUSE;
    else
        failure_mode = SPAWN_QUIT;

    /* review process fd limit */
    setup_fdlimit((odir == NULL) ? 3 : 5, max);

    /* Allocate and initialize the control structures */
    pfd = (struct pollfd *) malloc((max+2)*3 * sizeof(struct pollfd));
    if (pfd == NULL)
      {
	perror("malloc failed");
	return RC_ERROR;
      }
    memset((void *) pfd, 0, (max+2)*3 * sizeof(struct pollfd));
    idx = 0;
    while (idx < (max+2)*3)
	pfd[idx++].fd = -1;

    children = (struct child *) malloc((max+1) * sizeof(struct child));
    if (children == NULL)
      {
	perror("malloc failed");
	free(pfd);
	return RC_ERROR;
      }
    memset((void *) children, 0, (max+1)*sizeof(struct child));

    /* Setup SIGINT handler */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = shmux_sigint;
    sigaction(SIGINT, &sa, &saved_sa);
    got_sigint = 0;

    /* Initialize the status module. */
    status_init(ping != NULL, test != 0, utest != ANALYZE_NONE);

    /* Run fping if requested */
    if (ping != NULL)
      {
	u_int count = 0;
	
	pfd[2].fd = -1;
	cargv[0] = "fping"; cargv[1] = "-t"; cargv[2] = ping; cargv[3] = NULL;
	children[0].pid = exec(&(pfd[0].fd), &(pfd[1].fd), &(pfd[2].fd),
			       NULL, cargv, 0);
	if (children[0].pid == -1)
	    /* Error message was given by exec() */
	    spawn_mode = SPAWN_FATAL;
	else
	  {
	    init_child(&(children[0]));

	    pfd[1].events = POLLIN;
	    pfd[2].events = POLLIN;

	    while (target_next(1) == 0)
	      {
                char *tname;

		target_start();
		count += 1;
                tname = strchr(target_getname(), '@');
                if (tname == NULL)
                    tname = target_getname();
                else
                    tname += 1;
		write(pfd[0].fd, tname, strlen(tname));
		write(pfd[0].fd, "\n", 1);
	      }
	    close(pfd[0].fd); pfd[0].fd = -1;
	    iprint("Pinging %u targets...", count);
	    dprint("fping pid = %d (idx=0) %d/%d/%d",
		   children[0].pid, pfd[0].fd, pfd[1].fd, pfd[2].fd);
	    ping = NULL;
	  }
      }
    else
	/* No fping, let's move on to the next phase then */
	while (target_next(1) == 0)
	  {
	    target_start();
	    target_result(1);
	  }

    /* From here on, it's one big loop. */
    while (spawn_mode != SPAWN_FATAL)
      {
	int pollrc, done;
	char *what;

	/* Update the status line before (possibly) pausing in poll() */
	status_update();

	/* Check (or not) for input */
	pfd[0].fd = tty_fd();
	if (pfd[0].fd >= 0)
#if !defined(BROKEN_POLL)
	    pfd[0].events = POLLIN;
#else
            pfd[0].events = 0;
#endif
	else
	  {
	    pfd[0].events = 0;
            if (spawn_mode == SPAWN_PAUSE)
                spawn_mode = failure_mode;
	  }

	/* Check for data to read/write */
	pollrc = poll(pfd, (max+2)*3, 250);
	if (pollrc == -1 && errno != EINTR)
	  {
	    perror("poll");
	    spawn_mode = SPAWN_FATAL;
	    break;
	  }
#if defined(BROKEN_POLL)
        if (pfd[0].fd >= 0)
          {
            pfd[0].revents = POLLIN;
            pollrc += 1;
          }
#endif

	/* Abort? */
	switch (got_sigint)
	  {
	  case 0:
	      break;
	  case 1:
              dprint("Sending SIGINT to all children..");
              idx = 0;
              while (idx < max+1)
                {
                  if (children[idx].pid > 0)
                      kill(-children[idx].pid, SIGINT);
                  idx += 1;
                }
	      eprint("Waiting for existing children to abort..");
	      got_sigint += 1;
	      /* FALLTHRU */
	  case 2:
	      spawn_mode = SPAWN_QUIT;
	      break;
	  default:
	      spawn_mode = SPAWN_ABORT;
	      break;
	  }

	/* read and process children output if any */
	if (pollrc > 0)
	  {
	    dprint("poll(%d) = %d", (max+2)*3, pollrc);
	    idx = 0;
	    while (idx < (max+2)*3)
	      {
		if (pfd[idx].fd == -1 || pfd[idx].revents == 0)
		  {
		    idx +=1;
		    continue;
		  }

		if (idx == 0)
		    what = "user";
		else if (idx < 3)
		    what = "fping";
		else
		  {
		    if (target_setbynum(children[idx/3].num) != 0)
			abort();
		    what = target_getname();
		  }

		/* Something is going on.. */
		dprint("idx=%d[%s] fd=%d(%d) IN=%d OUT=%d ERR=%d HUP=%d (%X)",
		       idx, what, pfd[idx].fd, idx%3,
		       (pfd[idx].revents & POLLIN) != 0,
		       (pfd[idx].revents & POLLOUT) != 0,
		       (pfd[idx].revents & POLLERR) != 0,
		       (pfd[idx].revents & POLLHUP) != 0,
                       pfd[idx].revents);

		if (idx % 3 != 0 || idx == 0)
		  {
		    /*
		    ** Stdout or stderr with output ready to be read,
		    ** or input to be read from the user.
		    */
		    char buffer[8192];
		    int sz;
        int err = 0;

		    sz = read(pfd[idx].fd, buffer, (idx == 0) ? 1 : 8191);
        if (sz < 0)
          err = errno;
        dprint("idx=%d[%s] fd=%d(%d) read()=%d",
			   idx, what, pfd[idx].fd, idx%3, sz);
		    if (sz > 0)
		      {
			buffer[sz] = '\0';
			if (idx == 0)
			    parse_user(buffer[0], children, max);
			else
			    parse_child(what, idx<=2, test<0, utest,
					children+(idx/3), idx%3, buffer);
		      }
		    else
		      {
			if (idx == 0)
			  {
#if !defined(BROKEN_POLL)
			    if (sz == 0)
				eprint("Unexpected empty read(/dev/tty) result");
			    else
				eprint("Unexpected read(/dev/tty) error for: %s",
				       strerror(err));
			    if (sz == 0 || err != EINTR) {
				tty_restore();
          }
#else
			    if (sz != 0)
				eprint("Unexpected read(/dev/tty) error for: %s",
				       strerror(err));
			    if (err != 0 && err != EINTR) {
				tty_restore();
          }
#endif
			  }
			else
			  {
			    char **left;

			    /*
			    ** Child is probably gone, we'll catch that below;
			    ** For now, just cleanup.
			    */
			    if (sz == -1)
				eprint("Unexpected read(STD%s) error for %s: %s",
				       (idx%3 == 1) ? "OUT" : "ERR", what,
				       strerror(errno));
			    close(pfd[idx].fd); pfd[idx].fd = -1;
			    if (idx%3 == 1)
				left = &(children[idx/3].obuf);
			    else
				left = &(children[idx/3].ebuf);
			    if (*left != NULL)
			      {
				tprint(what, (idx%3 == 1) ? MSG_STDOUTTRUNC
				       : MSG_STDERRTRUNC, "%s", *left);
				eprint("Previous line was incomplete.");/*So?*/
				free(*left);
				*left = NULL;
			      }
			  }
		      }
		  }
		else
		  {
		    /* Stdin ready to be written to */
		    abort(); /* we don't use this yet, so how did we get here? */
		  }

		idx += 1;
	      }
	  }

	/* Shall we abort? */
	if ( spawn_mode == SPAWN_ABORT )
	    break;

	/* Check on the status of children & spawn more as needed */
	idx = 0; done = 1;
	while (idx < max+1)
	  {
	    int status, wprc;

	    /* Spawn as many processes as allowed */
	    if (children[idx].pid <= 0)
	      {
		/* Available slot to spawn a new child */
		
		if (spawn_mode == SPAWN_QUIT)
		  {
		    /* Don't spawn any more children */
		    idx += 1;
		    continue;
		  }

		/* Spawn phase 4 ready first */
		if (idx > 0 && target_next(4) == 0)
		  {
		    if (utest != ANALYZE_RUN)
		      {
			dprint("%s skipped external analyzer",
			       target_getname());
			target_start();
			target_result(1);
			continue;
		      }

		    done = 0;

		    if (spawn_mode == SPAWN_PAUSE)
		      {
			/* Don't spawn any more children */
			idx += 1;
			continue;
		      }

		    target_start();

		    init_child(&(children[idx]));
		    children[idx].analyzer = 1;
		    children[idx].output = outmode & (OUT_MIXED|OUT_ATEND);
		    assert( odir != NULL && (outmode & OUT_COPY) != 0 );
		    children[idx].ofile = output_file(&children[idx].ofname, odir, target_getname(), "analyzer.stdout");
		    if (children[idx].ofile == -1)
		      {
			eprint("Fatal error for %s", target_getname());
			target_result(-1);
			continue;
		      }
		    children[idx].efile = output_file(&children[idx].efname, odir, target_getname(), "analyzer.stderr");
		    if (children[idx].efile == -1)
		      {
			close(children[idx].efile);
			eprint("Fatal error for %s", target_getname());
			target_result(-1);
			continue;
		      }
		    pfd[idx*3].fd = -1;
		    cargv[0] = analyzer_cmd();
		    cargv[1] = target_getname();
		    cargv[2] = odir;
		    cargv[3] = NULL;
		    children[idx].pid = exec(NULL, &(pfd[idx*3+1].fd),
					     &(pfd[idx*3+2].fd),
					     target_getname(), cargv,
					     analyzer_timeout());
		    if (children[idx].pid == -1)
			  {
			    /* Error message was given by exec() */
			    eprint("Fatal error for %s", target_getname());
			    target_result(-1);
			    continue;
			  }

		    pfd[idx*3+1].events = POLLIN;
		    pfd[idx*3+2].events = POLLIN;

		    dprint("%s, phase 4: pid = %d (idx=%d) %d/%d/%d",
			   target_getname(), children[idx].pid, idx,
			   pfd[idx*3].fd, pfd[idx*3+1].fd, pfd[idx*3+2].fd);
		    idx += 1;
		    continue;
		  }

		/* Spawn phase 3 ready */
		if (idx > 0 && spawn_mode != SPAWN_NONE && target_next(3) == 0)
		  {
		    done = 0;

		    if (spawn_mode == SPAWN_PAUSE)
		      {
			/* Don't spawn any more children */
			idx += 1;
			continue;
		      }

		    target_start();

		    init_child(&(children[idx]));

		    children[idx].output = outmode;
		    if (spawn_mode == SPAWN_ONE)
		      {
			spawn_mode = SPAWN_NONE;
			if ((outmode & OUT_ATEND) != 0
			    && (outmode & OUT_IFERR) == 0)
			    children[idx].output = (outmode & ~OUT_ATEND)
				|OUT_MIXED;
		      }

		    if ((outmode & (OUT_ATEND|OUT_IFERR|OUT_COPY)) != 0)
		      {
			assert( odir != NULL );
			children[idx].ofile = output_file(&children[idx].ofname, odir, target_getname(), "stdout");
			if (children[idx].ofile == -1)
			  {
			    eprint("Fatal error for %s", target_getname());
			    target_result(-1);
			    continue;
			  }
			children[idx].efile = output_file(&children[idx].efname, odir, target_getname(), "stderr");
			if (children[idx].efile == -1)
			  {
			    close(children[idx].ofile);
			    eprint("Fatal error for %s", target_getname());
			    target_result(-1);
			    continue;
			  }
		      }
		    pfd[idx*3].fd = -1;
		    children[idx].pid = exec(NULL, &(pfd[idx*3+1].fd),
					     &(pfd[idx*3+2].fd),
					     target_getname(),
                                             target_getcmd(cmd),
					     ctimeout);
		    if (children[idx].pid == -1)
			  {
			    /* Error message was given by exec() */
			    eprint("Fatal error for %s", target_getname());
			    target_result(-1);
			    continue;
			  }

		    if (ctimeout > 0)
			children[idx].timeout = time(NULL) + ctimeout + 5;

		    pfd[idx*3+1].events = POLLIN;
		    pfd[idx*3+2].events = POLLIN;

		    dprint("%s, phase 3: pid = %d (idx=%d) %d/%d/%d",
			   target_getname(), children[idx].pid, idx,
			   pfd[idx*3].fd, pfd[idx*3+1].fd, pfd[idx*3+2].fd);
		    idx += 1;
		    continue;
		  }

		/* Spawn phase 2 ready last */
		if (idx > 0 && children[idx].pid <= 0 && target_next(2) == 0)
		  {
		    if (test == 0)
		      {
			dprint("%s skipped test", target_getname());
			target_start();
			target_result(1);
			continue;
		      }

		    done = 0;

		    if (spawn_mode == SPAWN_PAUSE)
		      {
			/* Don't spawn any more children */
			idx += 1;
			continue;
		      }

		    target_start();

		    pfd[idx*3].fd = -1;
		    children[idx].pid = exec(NULL, &(pfd[idx*3+1].fd),
					     &(pfd[idx*3+2].fd),
					     target_getname(),
                                             target_getcmd("echo SHMUX."),
					     abs(test));

		    if (children[idx].pid == -1)
		      {
			/* Error message was given by exec() */
			eprint("Fatal error for %s", target_getname());
			target_result(-1);
			continue;
		      }

		    pfd[idx*3+1].events = POLLIN;
		    pfd[idx*3+2].events = POLLIN;

		    init_child(&(children[idx]));
		    children[idx].test = 1;
		    dprint("%s, phase 2: pid = %d (idx=%d) %d/%d/%d",
			   target_getname(), children[idx].pid, idx,
			   pfd[idx*3].fd, pfd[idx*3+1].fd,pfd[idx*3+2].fd);
		    idx += 1;
		    continue;
		  }

		/* Nothing left to do! */
		idx += 1;
		continue;
	      }

	    /* Existing child */
	    done = 0;
	    if (idx == 0)
		what = "fping";
	    else
	      {
		if (target_setbynum(children[idx].num) != 0)
		    abort();
		what = target_getname();
	      }
	    
	    if (children[idx].status >= 0)
	      {
		/* restore saved status */
		wprc = children[idx].pid;
		status = children[idx].status;
	      }
	    else
              {
		/* get current status */
                int saved;

		wprc = waitpid(children[idx].pid, &status, WNOHANG|WUNTRACED);
                saved = errno;
		if (wprc == -1)
                  {
		    eprint("waitpid(%d[%s]): %s",
                           children[idx].pid, what, strerror(saved));
                    if (saved == ECHILD)
                      {
                        /* this shouldn't happen, but has been seen. */
                        eprint("Lost track of %s: exit status unavailable!",
                               what);
                        wprc = children[idx].pid;
                        status = 0;
                      }
                  }
              }

	    if (wprc <= 0 || children[idx].status >= 0)
	      {
		/*
                ** child is either alive and well
                ** or dead but with open fds (probably alive grandchildren),
                ** timeout exceeded?
                */
		if (children[idx].timeout != 0
		    && time(NULL) > children[idx].timeout)
		  {
		    assert( children[idx].timedout == 0 ||
			    children[idx].timedout == 1 );
		    if (children[idx].timedout == 0)
		      {
			iprint("Time out for %s (Sending SIGTERM)..", what);
			kill(-children[idx].pid, SIGTERM);
			children[idx].timeout = time(NULL) + 5;
		      }
		    else
		      {
			iprint("Time out for %s (Sending SIGKILL)..", what);
			kill(-children[idx].pid, SIGKILL);
			children[idx].timeout = 0;
		      }
		    children[idx].timedout += 1;
		  }

                if (wprc <= 0)
                  {
                    /* Live children */
                    idx += 1;
                    continue;
                  }

                /* Already dead kids need to be looked at more thoroughly */
	      }

	    if (WIFSTOPPED(status) != 0)
	      {
		/*
		** Child is stopped/suspended.  This probably isn't normal
		** or expected, unless it was self inflicted after fork(),
		** see exec.c
		*/
		/*
		** YYY These could/should be ignored once we've received
		** some output from the child.
		*/
		if (WSTOPSIG(status) == SIGTSTP)
		  {
		    /* exec() failed, see exec.c */
		    dprint("%s (idx=%d) stopped on SIGTSTP, sending SIGCONT.",
			   what, idx);
		    children[idx].execstate = 1;
		    kill(-children[idx].pid, SIGCONT);
		  }
		else
		    eprint("%s for %s stopped: %s!?",
			   (children[idx].test == 0) ? 
			   (children[idx].analyzer == 0 ) ? "Child"
			   : "Analyzer" : "Test", what,
			   strsignal(WSTOPSIG(status)));
		idx += 1;
		continue;
	      }

	    /* XXX what about +0 ?  needed here or not? need to test! */
	    if (pfd[idx*3+1].fd != -1 || pfd[idx*3+2].fd !=-1)
	      {
		/* Let's finish reading from these before going on */
		if (children[idx].status == -1)
                  {
		    dprint("%s (idx=%d) died but has open fd(s), saved status",
			   what, idx);
                    if (WTERMSIG(status) == SIGALRM)
                      {
                        /* Could be stray grand children */
                        dprint("%s (idx=%d) died from SIGALRM, signaling process group", what, idx);
                        kill(-children[idx].pid, SIGALRM);
                      }
                  }
		children[idx].status = status;
		idx += 1;
		continue;
	      }

	    /*
	    ** This point is reached when the child's stdout and stderr
	    ** have both been closed (following a read()).
	    */
	    if (pfd[idx*3].fd != -1)
	      {
		/* Time to close stdin (which we don't use for now anyways) */
		if (idx != 0)
		  {
		    close(pfd[idx*3].fd);
		    pfd[idx*3].fd = -1;
		  }
	      }

            /*
            ** Check for orphans whom we've lost contact with
            */
            if (kill(-children[idx].pid, 0) == 0)
              {
                /* Is waiting for these really wise/necessary? */
                if (time(NULL) - children[idx].orphan > 15)
                  {
                    if (children[idx].orphan == 0)
                      {
                        dprint("%s (idx=%d) has left orphan(s), saved status, waiting...", what, idx);
                        children[idx].status = status;
                      }
                    else
                        dprint("%s (idx=%d) has left orphan(s), waiting...",
                               what, idx);
                    children[idx].orphan = time(NULL);
                  }
                idx += 1;
                continue;
              }

	    /*
	    ** If user asked for a non-mixed output, now's a good time to
	    ** show the output on screen.
	    */
	    if (children[idx].ofile != -1)
	      {
		if ((children[idx].output & OUT_ATEND) != 0
		    && (children[idx].output & OUT_IFERR) == 0)
		    output_show(what, children[idx].ofile,
				children[idx].ofname,1);
		if ((outmode & OUT_COPY) == 0
		    && unlink(children[idx].ofname) == -1)
		    eprint("unlink(%s): %s",
			   children[idx].ofname, strerror(errno));
	      }
	    if (children[idx].efile != -1)
	      {
		if ((children[idx].output & OUT_ATEND) != 0
		    && (children[idx].output & OUT_IFERR) == 0)
		    output_show(what, children[idx].efile,
				children[idx].ofname,2);
		if ((outmode & OUT_COPY) == 0
		    && unlink(children[idx].efname) == -1)
		    eprint("unlink(%s): %s",
			   children[idx].efname, strerror(errno));
	      }

	    if (idx > 0)
		if (target_setbynum(children[idx].num) != 0)
		    abort();

	    /* Check and optionally report the exit status */
	    if (WIFEXITED(status) != 0)
	      {
		if (children[idx].test == 1)
		    dprint("Test for %s exited with status %d",
			   what, WEXITSTATUS(status));
		else if (children[idx].analyzer == 1)
		  {
		    dprint("Analyzer for %s exited with status %d",
			   what, WEXITSTATUS(status));
		    if (WEXITSTATUS(status) == 0)
		      {
			iprint("Analysis of %s output indicates a success", what);
			set_cmdstatus(CMD_SUCCESS);
		      }
		    else
		      {
			eprint("Analysis of %s output indicates an error", what);
			set_cmdstatus(CMD_ERROR);
		      }
		  }
		else if (idx == 0)
		  {
		    /* fping */
		    if (WEXITSTATUS(status) > 2
			&& children[idx].execstate == 0)
			eprint("Child for %s exited with status %d",
			       what, WEXITSTATUS(status));
		  } 
		else if (children[idx].execstate == 0)
		  {
		    /* save exit status */
		    if ((outmode & OUT_COPY) != 0)
		      {
			int fd;
			char *fn;

			assert( odir != NULL );
			fd = output_file(&fn, odir, what, "exit");
			if (fd >= 0)
			  {
			    char buf[4];
			    snprintf(buf, sizeof(buf), "%u",
				WEXITSTATUS(status));
			    write(fd, buf, strlen(buf));
			    close(fd);
			    free(fn);
			  }
		      }

		    if (byteset_test(BSET_ERROR, WEXITSTATUS(status)) == 0)
		      {
			if ((children[idx].output & OUT_IFERR) != 0)
			  {
			    output_show(what, children[idx].ofile,
					children[idx].ofname, 1);
			    output_show(what, children[idx].efile,
					children[idx].ofname, 2);
			  }
			set_cmdstatus(CMD_ERROR);
			eprint("Child for %s exited with status %d",
			       what, WEXITSTATUS(status));
		      }
		    else
		      {
			if (utest == ANALYZE_NONE || utest == ANALYZE_RUN)
			    set_cmdstatus(CMD_SUCCESS);
			else if (utest == ANALYZE_LNRE
				 || utest == ANALYZE_LNPCRE)
			  {
			    if ((children[idx].output & OUT_ERR) == 0)
				set_cmdstatus(CMD_SUCCESS);
			    else
				set_cmdstatus(CMD_ERROR);
			  }
			else
			  {
			    /*
			    ** Analyze the output to tell success from failure
			    ** based on user supplied criteria.
			    */
			    if (analyzer_run(utest,
					     children[idx].ofile,
					     children[idx].ofname,
					     children[idx].efile,
					     children[idx].efname) == 0)
			      {
				iprint("Analysis of %s output indicates a success", what);
				set_cmdstatus(CMD_SUCCESS);
			      }
			    else
			      {
				eprint("Analysis of %s output indicates an error", what);
				if ((children[idx].output & OUT_IFERR) != 0)
				  {
				    output_show(what, children[idx].ofile,
						children[idx].ofname, 1);
				    output_show(what, children[idx].efile,
						children[idx].ofname, 2);
				  }
				set_cmdstatus(CMD_ERROR);
			      }
			  }
			if (byteset_test(BSET_SHOW, WEXITSTATUS(status)) == 0)
			    tprint(myname, MSG_STDOUT,
				   "Child for %s exited with status %d",
				   what, WEXITSTATUS(status));
			else
			    iprint("Child for %s exited (with status %d)",
				   what, WEXITSTATUS(status));
		      }
		  }
		else
		    set_cmdstatus(CMD_FAILURE);

		/* If outputing to a file, clean things up. */
		if (children[idx].ofile != -1)
		  {
		    close(children[idx].ofile);
		    assert( children[idx].ofname != NULL );
		    free(children[idx].ofname);
		  }
		if (children[idx].efile != -1)
		  {
		    close(children[idx].efile);
		    assert( children[idx].efname != NULL );
		    free(children[idx].efname);
		  }
	      } else {
		assert( WTERMSIG(status) != 0 );
		if (WTERMSIG(status) == SIGALRM
		    || (children[idx].timedout > 0
			&& (WTERMSIG(status) == SIGTERM
			    || WTERMSIG(status) == SIGKILL)))
		    if (children[idx].test == 0)
		      {
			eprint("%s for %s timed out (%s)",
			       (children[idx].analyzer == 0) ? "Child"
			       : "Analyzer", what,
			       strsignal(WTERMSIG(status)));
			if (idx > 0)
			    set_cmdstatus(CMD_TIMEOUT);
		      }
		    else
		      {
			assert( WTERMSIG(status) == SIGALRM );
			children[idx].passed = -2;
		      }
		else
		  {
		    eprint("%s for %s died: %s%s",
			   (children[idx].test == 0) ? 
			   (children[idx].analyzer == 0 ) ? "Child"
			   : "Analyzer" : "Test", what,
			   strsignal(WTERMSIG(status)),
			   (WCOREDUMP(status) != 0) ? " (core dumped)" : "");
		    if (idx > 0 && children[idx].test == 0)
			set_cmdstatus(CMD_ERROR);
		  }
	      }

	    /* mark the slot as free */
	    children[idx].pid = 0;

	    if (idx == 0)
              {
		dprint("fping is done");
                while (target_pong(NULL) == 0)
                  {
                    eprint("%s assumed to be alive (missing from fping results)", target_getname());
                    target_result(1);
                  }
              }
	    else
	      {
		if (children[idx].execstate != 0
		    || (children[idx].test == 1 && children[idx].passed != 1))
		  {
		    if (children[idx].test == 1)
			eprint("Test %s for %s",
			       (children[idx].passed == -2) ? "timed out"
			       : "failed", what);
		    target_result(-1);
		  }
		else
		    target_result(1);
	      }

	    status_spawned(-1);

	    /* Don't increment idx, catch it at the top again */
	  }

	if (done == 1)
	    break;
      }

    /* Restore saved SIGINT handler */
    sigaction(SIGINT, &saved_sa, NULL);

    free(children); /* XXX Leak */
    free(pfd);

    sprint("");

    switch (spawn_mode)
      {
      case SPAWN_FATAL: return RC_FATAL;
      case SPAWN_ABORT: return RC_ABORT;
      case SPAWN_QUIT:  return RC_QUIT;
      default:          return RC_OK;
      }
}
