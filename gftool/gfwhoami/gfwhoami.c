/*
 * $Id: gfwhoami.c 7161 2012-11-23 14:15:41Z tatebe $
 */

#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <gfarm/gfarm.h>
#ifdef HAVE_GSI
#include "auth.h"
#endif
#include "gfarm_path.h"

char *program_name = "gfwhoami";

#ifdef HAVE_GSI
#define GFWHOAMI_NO_ARG_OPTIONS	"vh"
#define GFWHOAMI_GETOPT_ARG	"P:hv?"
#else
#define GFWHOAMI_NO_ARG_OPTIONS	"h"
#define GFWHOAMI_GETOPT_ARG	"P:h?"
#endif

void
usage(void)
{
	fprintf(stderr,
	    "Usage: %s [-" GFWHOAMI_NO_ARG_OPTIONS "] [-P <path>]\n",
	    program_name);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	gfarm_error_t e;
	int c;
	const char *path = ".";
	char *user, *realpath = NULL;
#ifdef HAVE_GSI
	int verbose_flag = 0;
#endif

	if (argc > 0)
		program_name = basename(argv[0]);

	e = gfarm_initialize(&argc, &argv);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: gfarm_initialize: %s\n", program_name,
			gfarm_error_string(e));
		exit(EXIT_FAILURE);
	}

	while ((c = getopt(argc, argv, GFWHOAMI_GETOPT_ARG)) != -1) {
		switch (c) {
		case 'P':
			path = optarg;
			break;
#ifdef HAVE_GSI
		case 'v':
			verbose_flag = 1;
			break;
#endif
		case 'h':
		case '?':
		default:
			usage();
		}
	}

	if (argc - optind > 0)
		usage();

	if (gfarm_realpath_by_gfarm2fs(path, &realpath) == GFARM_ERR_NO_ERROR)
		path = realpath;
	if ((e = gfarm_get_global_username_by_url(path, &user))
	    != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: gfarm_get_global_username_by_url: %s\n",
		    program_name, gfarm_error_string(e));
		exit(EXIT_FAILURE);
	}
	free(realpath);
	printf("%s", user);
	free(user);
#ifdef HAVE_GSI
	if (verbose_flag)
		printf(" %s", gfarm_gsi_client_cred_name());
#endif
	printf("\n");

	e = gfarm_terminate();
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: gfarm_terminate: %s\n", program_name,
			gfarm_error_string(e));
		exit(EXIT_FAILURE);
	}
	return (0);
}
