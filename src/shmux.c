/*
** Copyright (C) 2002 Christophe Kalt
**
** This file is part of shmux,
** see the LICENSE file for details on your rights.
*/

#include "os.h"
#include <libgen.h>
#include <time.h>

#include "version.h"

#include "loop.h"
#include "target.h"
#include "term.h"

static char const rcsid[] = "@(#)$Id: shmux.c,v 1.3 2002-07-06 20:53:16 kalt Exp $";

extern char *optarg;
extern int optind, opterr;

char *myname;

#define	DEFAULT_MAXWORKERS 10
#define DEFAULT_PINGTIMEOUT "500"
#define DEFAULT_TESTTIMEOUT 15
#define DEFAULT_METHOD "ssh"

static void usage(int);

static void
usage(detailed)
int detailed;
{
    fprintf(stderr, "Usage: %s [ options ] -c <command> <host1> [ <host2> ... ]\n", myname);
/*    fprintf(stderr, "Usage: %s [ options ] -i [ <host1> [ <host2> ... ] ]\n", myname);*/
    if (detailed == 0)
	return;
    fprintf(stderr, "  -h            Print this message.\n");
    fprintf(stderr, "  -V            Output version info.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -M            Maximum number of simultaneous processes (Default: %u).\n", DEFAULT_MAXWORKERS);
    fprintf(stderr, "  -m <method>   Set the default method (Default: %s).\n", DEFAULT_METHOD);
    fprintf(stderr, "  -p            Ping targets to check for life.\n");
    fprintf(stderr, "  -P <millisec> Initial target timeout given to fping (Default: %s).\n", DEFAULT_PINGTIMEOUT);
    fprintf(stderr, "  -t            Send test command to verify target health.\n");
    fprintf(stderr, "  -T <seconds>  Time to wait for test answer (Default: %d).\n", DEFAULT_TESTTIMEOUT);
    fprintf(stderr, "\n");
    fprintf(stderr, "  -v            Verbose output including target names.\n");
    fprintf(stderr, "  -s            Suppress progress status.\n");
    fprintf(stderr, "  -q            Suppress final summary.\n");
    fprintf(stderr, "  -d            Display internal status messages.\n");
    fprintf(stderr, "  -D            Display internal debug messages.\n");
}

int
main(int argc, char **argv)
{
    int opt_verbose, opt_status, opt_quiet, opt_internal, opt_debug;
    int opt_maxworkers, opt_test, opt_vtest;
    char *opt_method, *opt_command, *opt_ping;
    int longest, ntargets;
    time_t start;

    myname = basename(argv[0]);

    opt_status = 1;
    opt_verbose = opt_quiet = opt_internal = opt_debug = 0;
    opt_maxworkers = DEFAULT_MAXWORKERS;
    opt_test = opt_vtest = 0;
    opt_method = getenv("SHMUX_SH");
    if (opt_method == NULL)
	opt_method = DEFAULT_METHOD;

    opt_command = opt_ping = NULL;

    opterr = 0;
    while (1)
      {
        int c;
	
        c = getopt(argc, argv, "c:dDhm:M:pP:qstT:vV");
	
        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c)
          {
	  case 'c':
	      opt_command = optarg;
	      break;
	  case 'd':
	      opt_internal = 1;
	      break;
	  case 'D':
	      opt_debug = 1;
	      break;
          case 'h':
              usage(1);
              exit(0);
              break;
	  case 'm':
	      opt_method = optarg;
	      break;
	  case 'M':
	      opt_maxworkers = atoi(optarg);
	      break;
	  case 'p':
	      if (opt_ping == NULL)
		  opt_ping = DEFAULT_PINGTIMEOUT;
	      break;
	  case 'P':
	      opt_ping = optarg;
	      break;
	  case 'q':
	      opt_quiet = 1;
	      break;
	  case 's':
	      opt_status = 0;
	      break;
	  case 't':
	      if (opt_test == 0)
		  opt_test = DEFAULT_TESTTIMEOUT;
	      opt_vtest += 1;
	      break;
	  case 'T':
	      opt_test = atoi(optarg);
	      opt_vtest += 1;
	      break;
	  case 'v':
	      opt_verbose = 1;
	      break;
	  case 'V':
	      printf("%s version %s\n", myname, SHMUX_VERSION);
	      exit(0);
	  case '?':
	      opterr += 1;
	      break;
	  default:
	      abort();
	  }
      }

    target_default(opt_method);
    if (opt_maxworkers <= 0)
      {
	fprintf(stderr, "%s: Invalid -M option!\n", myname);
	exit(1);
      }

    if (opt_vtest > 1)
	opt_test *= -1;

    if (optind >= argc || opterr > 0 || opt_command == NULL)
      {
        usage(0);
        exit(1);
      }

    ntargets = 0;
    longest = strlen(myname);
    while (optind < argc)
      {
	int length;

	length = target_add(argv[optind++]);
	if (length > longest)
	    longest = length;
	ntargets += 1;
      }

    term_init(longest, opt_verbose, opt_status, opt_internal, opt_debug);

    start = time(NULL);
    loop(opt_command, opt_maxworkers, opt_ping, opt_test);
      }
    /* Summary of results unless asked to be quiet */
    if (opt_quiet == 0)
      {
	nprint("%d targets processed in %d seconds.",
	       ntargets, (int) (time(NULL) - start));
	target_results();
	target_results((int) (time(NULL) - start));
      }

    exit(0);
}