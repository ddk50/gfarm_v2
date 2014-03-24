#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <gssapi.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>

#include <gfarm/gfarm_config.h>
#include <gfarm/error.h>

#include "gfnetdb.h"
#include "gfutil.h"

#include "tcputil.h"
#include "gfsl_config.h"
#include "gfarm_gsi.h"
#include "gfarm_auth.h"
#include "gfarm_secure_session.h"
#include "misc.h"

#include "scarg.h"

static char *hostname = NULL;

static int
ParseArgs(int argc, char **argv)
{
    int c;

    while ((c = getopt(argc, argv, "h:" COMMON_OPTIONS)) != -1) {
	switch (c) {
	case 'h':
	    hostname = optarg;
	    break;
	default:
	    if (HandleCommonOptions(c, optarg) != 0) {
		fprintf(stderr, "HandleCommonOptions(%s) failed.\n", optarg);
		return -1;
	    }
	    break;
	}
    }
    if (optind < argc) {
	fprintf(stderr, "unknown extra argument %s\n", argv[optind]);
	return -1;
    }
    
    return 0;
}


void	doServer(int fd, char *host, int port, gss_cred_id_t myCred,
		 gss_name_t acceptorName);

int
main(int argc, char **argv)
{
    int bindFd = -1;
    struct sockaddr_in remote;
    socklen_t remLen = sizeof(struct sockaddr_in);
    int fd = -1;
    OM_uint32 majStat, minStat;
    char myHostname[4096];
    gss_cred_id_t myCred;

    if (hostname == NULL) {
	if (gethostname(myHostname, 4096) < 0) {
	    perror("gethostname");
	    return 1;
	}
	hostname = myHostname;
    }

    gflog_auth_set_verbose(1);
    if (gfarmSecSessionInitializeBoth(NULL, NULL, NULL,
				      &majStat, &minStat) <= 0) {
	fprintf(stderr, "can't initialize as both role because of:\n");
	gfarmGssPrintMajorStatus(majStat);
	gfarmGssPrintMinorStatus(minStat);
	gfarmSecSessionFinalizeBoth();
	return 1;
    }

    if (ParseArgs(argc, argv) != 0) {
	fprintf(stderr, "parsing of argument failed.\n");
	return 1;
    }

    if (!acceptorSpecified) {
	if (gfarmGssImportNameOfHost(&acceptorName, hostname,
				     &majStat, &minStat) < 0) {
	    fprintf(stderr, "gfarmGssImportNameOfHost() failed.\n");
	    gfarmGssPrintMajorStatus(majStat);
	    gfarmGssPrintMinorStatus(minStat);
	    return 1;
	}
	myCred = GSS_C_NO_CREDENTIAL;
    } else {
	gss_name_t credName;
	char *credString;
        char *desired = newStringOfName(acceptorName);

	if (gfarmGssAcquireCredential(&myCred,
				      acceptorName, GSS_C_BOTH,
				      &majStat, &minStat, &credName) <= 0) {
	    fprintf(stderr, "Can't acquire credential for '%s' because of:\n",
                    desired);
	    gfarmGssPrintMajorStatus(majStat);
	    gfarmGssPrintMinorStatus(minStat);
	    return 1;
	}
        free(desired);
	credString = newStringOfName(credName);
	fprintf(stderr, "Acceptor Credential: '%s'\n", credString);
	free(credString);
	gfarmGssDeleteName(&credName, NULL, NULL);
    }

    /*
     * Create a channel.
     */
    bindFd = gfarmTCPBindPort(port);
    if (bindFd < 0) {
	gfarmSecSessionFinalizeBoth();
	fprintf(stderr, "Failed to bind port (%d)", port);
	return 1;
    }
    (void)gfarmGetNameOfSocket(bindFd, &port);
    fprintf(stderr, "Accepting port: %d\n", port);

    /*
     * Accept-fork loop.
     */
    while (1) {
	pid_t pid;
	char hbuf[NI_MAXHOST];

	fd = accept(bindFd, (struct sockaddr *)&remote, &remLen);
	if (fd < 0) {
	    perror("accept");
	    (void)close(bindFd);
	    return 1;
	}
	if (gfarm_getnameinfo((struct sockaddr *)&remote, remLen,
			      hbuf, sizeof(hbuf), NULL, 0, 0) == 0)
	    fprintf(stderr, "Connected from %s\n", hbuf);
	pid = fork();
	if (pid < 0) {
	    (void)close(fd);
	    (void)close(bindFd);
	    perror("fork");
	    return 1;
	} else if (pid == 0) {
	    (void)close(bindFd);
	    doServer(fd, hostname, port, myCred, acceptorName);
	    (void)close(fd);
	    exit(0);
	} else {
	    (void)close(fd);
	}
    }
#if 0 /* never reach here */
    gfarmSecSessionFinalizeBoth();
    return 0;
#endif
}
