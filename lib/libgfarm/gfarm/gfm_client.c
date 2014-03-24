#include <pthread.h>
#include <assert.h>
#include <stdio.h> /* for config.h */
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <netdb.h>
#include <sys/time.h>

#include <gfarm/gfarm_config.h>
#include <gfarm/gflog.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>
#include <gfarm/host_info.h>
#include <gfarm/user_info.h>
#include <gfarm/group_info.h>

#include "gfutil.h"
#include "hash.h"
#include "gfnetdb.h"
#include "lru_cache.h"
#include "queue.h"
#include "thrsubr.h"

#include "context.h"
#include "gfp_xdr.h"
#include "io_fd.h"
#include "sockopt.h"
#include "sockutil.h"
#include "host.h"
#include "auth.h"
#include "config.h"
#include "conn_cache.h"
#include "gfm_proto.h"
#include "gfj_client.h"
#include "xattr_info.h"
#include "gfm_client.h"
#include "fsngroup_info.h"
#include "quota_info.h"
#include "metadb_server.h"
#include "filesystem.h"
#include "liberror.h"
#include "nanosec.h"

struct gfm_connection {
	struct gfp_cached_connection *cache_entry;

	struct gfp_xdr *conn;
	enum gfarm_auth_method auth_method;

	/* parallel process signatures */
	gfarm_pid_t pid;
	char pid_key[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];

	struct gfarm_metadb_server *real_server;

	int failover_count;
};

#define staticp	(gfarm_ctxp->gfm_client_static)

struct gfm_client_static {
	struct gfp_conn_cache server_cache;
};

#define SERVER_HASHTAB_SIZE	31	/* prime number */

/* retry count for auth/process_alloc */
#define CONNERR_RETRY_COUNT 3

static gfarm_error_t gfm_client_connection_dispose(void *);

gfarm_error_t
gfm_client_static_init(struct gfarm_context *ctxp)
{
	struct gfm_client_static *s;

	GFARM_MALLOC(s);
	if (s == NULL)
		return (GFARM_ERR_NO_MEMORY);

	gfp_conn_cache_init(&s->server_cache,
		gfm_client_connection_dispose,
		"gfm_connection",
		SERVER_HASHTAB_SIZE,
		&ctxp->gfmd_connection_cache);

	ctxp->gfm_client_static = s;
	return (GFARM_ERR_NO_ERROR);
}

void
gfm_client_static_term(struct gfarm_context *ctxp)
{
	struct gfm_client_static *s = ctxp->gfm_client_static;

	if (s == NULL)
		return;

	gfp_conn_cache_term(&s->server_cache);
	free(s);
}

int
gfm_client_is_connection_error(gfarm_error_t e)
{
	return (IS_CONNECTION_ERROR(e));
}

struct gfp_xdr *
gfm_client_connection_conn(struct gfm_connection *gfm_server)
{
	return (gfm_server->conn);
}

int
gfm_client_connection_fd(struct gfm_connection *gfm_server)
{
	return (gfp_xdr_fd(gfm_server->conn));
}

enum gfarm_auth_method
gfm_client_connection_auth_method(struct gfm_connection *gfm_server)
{
	return (gfm_server->auth_method);
}

int
gfm_client_is_connection_valid(struct gfm_connection *gfm_server)
{
	return (gfp_is_cached_connection(gfm_server->cache_entry));
}

const char *
gfm_client_hostname(struct gfm_connection *gfm_server)
{
	return (gfp_cached_connection_hostname(gfm_server->cache_entry));
}

const char *
gfm_client_username(struct gfm_connection *gfm_server)
{
	return (gfp_cached_connection_username(gfm_server->cache_entry));
}

gfarm_error_t
gfm_client_set_username_for_gsi(struct gfm_connection *gfm_server,
	const char *username)
{
	return (gfp_cached_connection_set_username(gfm_server->cache_entry,
		username));
}

int
gfm_client_port(struct gfm_connection *gfm_server)
{
	return (gfp_cached_connection_port(gfm_server->cache_entry));
}

gfarm_error_t
gfm_client_source_port(struct gfm_connection *gfm_server, int *portp)
{
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);

	if (getsockname(gfm_client_connection_fd(gfm_server),
	    (struct sockaddr *)&sin, &slen) != 0) {
		*portp = 0;
		return (gfarm_errno_to_error(errno));
	} else {
		*portp = (int)ntohs(sin.sin_port);
		return (GFARM_ERR_NO_ERROR);
	}
}

struct gfarm_metadb_server*
gfm_client_connection_get_real_server(struct gfm_connection *gfm_server)
{
	return (gfm_server->real_server);
}

int
gfm_client_connection_failover_count(struct gfm_connection *gfm_server)
{
	return (gfm_server->failover_count);
}

gfarm_error_t
gfm_client_process_get(struct gfm_connection *gfm_server,
	gfarm_int32_t *keytypep, const char **sharedkeyp,
	size_t *sharedkey_sizep, gfarm_pid_t *pidp)
{
	if (gfm_server->pid == 0) {
		gflog_debug(GFARM_MSG_1001092,
			"server pid is invalid: %s",
			gfarm_error_string(GFARM_ERR_NO_SUCH_OBJECT));
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}

	*keytypep = GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET;
	*sharedkeyp = gfm_server->pid_key;
	*sharedkey_sizep = GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET;
	*pidp = gfm_server->pid;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_process_is_set(struct gfm_connection *gfm_server)
{
	return (gfm_server->pid != 0);
}

/* this interface is exported for a use from a private extension */
void
gfm_client_purge_from_cache(struct gfm_connection *gfm_server)
{
	gfp_cached_connection_purge_from_cache(&staticp->server_cache,
	    gfm_server->cache_entry);
}

#define gfm_client_connection_used(gfm_server) \
	gfp_cached_connection_used(&staticp->server_cache, \
	    (gfm_server)->cache_entry)

int
gfm_cached_connection_had_connection_error(struct gfm_connection *gfm_server)
{
	/* i.e. gfm_client_purge_from_cache() was called due to an error */
	return (!gfp_is_cached_connection(gfm_server->cache_entry));
}

void
gfm_client_connection_gc(void)
{
	gfp_cached_connection_gc_all(&staticp->server_cache);
}

#ifndef __KERNEL__	/* gfm_client_nonblock_sock_connect :: in user mode */

static gfarm_error_t
gfm_client_nonblock_sock_connect(const char *hostname, int port,
	const char *source_ip, int *sockp, struct addrinfo **aip)
{
	gfarm_error_t e;
	int sock, save_errno;
	struct addrinfo hints, *res;
	char sbuf[NI_MAXSERV];

	snprintf(sbuf, sizeof(sbuf), "%u", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;
	if (gfarm_getaddrinfo(hostname, sbuf, &hints, &res) != 0) {
		gflog_debug(GFARM_MSG_1001093,
			"getaddrinfo(%s) failed: %s",
			hostname,
			gfarm_error_string(GFARM_ERR_UNKNOWN_HOST));
		return (GFARM_ERR_UNKNOWN_HOST);
	}

	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock == -1 && (errno == ENFILE || errno == EMFILE)) {
		gfm_client_connection_gc(); /* XXX FIXME: GC all descriptors */
		sock = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
	}
	if (sock == -1) {
		save_errno = errno;
		gflog_debug(GFARM_MSG_1001094,
			"creation of socket failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}
	fcntl(sock, F_SETFD, 1); /* automatically close() on exec(2) */
	fcntl(sock, F_SETFL, O_NONBLOCK);

	/* XXX - how to report setsockopt(2) failure ? */
	gfarm_sockopt_apply_by_name_addr(sock,
	    res->ai_canonname, res->ai_addr);

	if (source_ip != NULL) {
		e = gfarm_bind_source_ip(sock, source_ip);
		if (e != GFARM_ERR_NO_ERROR) {
			close(sock);
			gfarm_freeaddrinfo(res);
			gflog_debug(GFARM_MSG_1001095,
				"bind(%s) failed: %s",
				source_ip,
				gfarm_error_string(e));
			return (e);
		}
	}

	if (connect(sock, res->ai_addr, res->ai_addrlen) < 0 &&
	    errno != EINPROGRESS) {
		save_errno = errno;
		close(sock);
		gfarm_freeaddrinfo(res);
		gflog_debug(GFARM_MSG_1001096, "connect failed: %s",
			strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}

	fcntl(sock, F_SETFL, 0); /* clear O_NONBLOCK */
	*sockp = sock;
	*aip = res;
	return (GFARM_ERR_NO_ERROR);
}
#endif /* !__KERNEL__ */

struct gfm_client_connect_info {
	int ms_idx;
	struct pollfd *pfd;
	struct addrinfo *res_ai;
	struct gfarm_metadb_server *ms;
};

#ifndef __KERNEL__	/* gfm_alloc_connect_info :: in user mode */
static gfarm_error_t
gfm_alloc_connect_info(int n, struct gfm_client_connect_info **cisp,
	struct pollfd **pfdsp)
{
	gfarm_error_t e;
	struct pollfd *pfds;
	struct gfm_client_connect_info *cis;

	GFARM_MALLOC_ARRAY(cis, n);
	GFARM_MALLOC_ARRAY(pfds, n);
	if (cis == NULL || pfds == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002571,
		    "%s", gfarm_error_string(e));
		free(cis);
		free(pfds);
		return (e);
	}

	*cisp = cis;
	*pfdsp = pfds;
	return (GFARM_ERR_NO_ERROR);
}


static gfarm_error_t
gfm_client_connect_single(const char *hostname, int port,
	const char *source_ip, struct gfm_client_connect_info **cisp,
	struct pollfd **pfdsp, int *nfdp)
{
	gfarm_error_t e;
	int sock, i, nmsl;
	struct addrinfo *res;
	struct pollfd *pfd, *pfds;
	struct gfm_client_connect_info *ci, *cis;
	struct gfarm_filesystem *fs;
	struct gfarm_metadb_server *ms, **msl;

	if ((e = gfm_alloc_connect_info(1, &cis, &pfds))
	    != GFARM_ERR_NO_ERROR)
		return (e);
	if ((e = gfm_client_nonblock_sock_connect(hostname, port,
	    source_ip, &sock, &res)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002572,
		    "%s", gfarm_error_string(e));
		free(pfds);
		free(cis);
		return (e);
	}
	ci = &cis[0];
	ci->res_ai = res;

	ci->ms = NULL;
	if ((fs = gfarm_filesystem_get(hostname, port)) != NULL) {
		msl = gfarm_filesystem_get_metadb_server_list(fs, &nmsl);
		assert(msl);
		for (i = 0; i < nmsl; ++i) {
			ms = msl[i];
			if (strcmp(gfarm_metadb_server_get_name(ms), hostname)
			    == 0 &&
			    gfarm_metadb_server_get_port(ms) == port) {
				assert(!gfarm_metadb_server_is_self(ms));
				ci->ms = ms;
				break;
			}
		}
	}

	pfd = &pfds[0];
	ci->pfd = pfd;
	pfd->fd = sock;
	pfd->events = POLLIN;
	*cisp = cis;
	*pfdsp = pfds;
	*nfdp = 1;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfm_client_connect_multiple(const char *hostname, int port,
	const char *source_ip, struct gfm_client_connect_info **cisp,
	struct pollfd **pfdsp, int *nfdp)
{
	gfarm_error_t e, e2 = GFARM_ERR_NO_ERROR;
	int i, nmsl, sock, nfd = 0;
	struct gfarm_filesystem *fs;
	struct gfarm_metadb_server *ms, **msl;
	struct addrinfo *res;
	struct pollfd *pfd, *pfds;
	struct gfm_client_connect_info *ci, *cis;

	fs = gfarm_filesystem_get(hostname, port);
	if (fs == NULL)
		return (gfm_client_connect_single(hostname, port,
		    source_ip, cisp, pfdsp, nfdp));
	msl = gfarm_filesystem_get_metadb_server_list(
	    fs, &nmsl);
	assert(msl);
	if ((e = gfm_alloc_connect_info(nmsl, &cis, &pfds))
	    != GFARM_ERR_NO_ERROR)
		return (e);
	for (i = 0; i < nmsl; ++i) {
		ms = msl[i];
		hostname = gfarm_metadb_server_get_name(ms);
		port = gfarm_metadb_server_get_port(ms);
		if (!gfarm_metadb_server_is_self(ms) &&
		    (e = gfm_client_nonblock_sock_connect(hostname, port,
		    source_ip, &sock, &res)) == GFARM_ERR_NO_ERROR) {
			ci = &cis[nfd];
			ci->ms = ms;
			ci->res_ai = res;
			pfd = &pfds[nfd];
			ci->pfd = pfd;
			pfd->fd = sock;
			pfd->events = POLLIN;
			++nfd;
		} else
			e2 = e;
	}
	if (nfd == 0) {
		assert(e2 != GFARM_ERR_NO_ERROR);
		return (e2);
	}
	*cisp = cis;
	*pfdsp = pfds;
	*nfdp = nfd;
	return (GFARM_ERR_NO_ERROR);
}
#else /* __KERNEL__ */
#define gfm_client_connect_multiple	NULL
#endif /* __KERNEL__ */

#ifdef HAVE_GSI
static gfarm_error_t
gfarm_set_global_user_by_gsi(struct gfm_connection *gfm_server)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	struct gfarm_user_info user;
	char *gsi_dn;

	/* Global user name determined by the distinguished name. */
	gsi_dn = gfarm_gsi_client_cred_name();
	if (gsi_dn != NULL) {
		e = gfm_client_user_info_get_by_gsi_dn(gfm_server,
			gsi_dn, &user);
		if (e == GFARM_ERR_NO_ERROR) {
			e = gfm_client_set_username_for_gsi(gfm_server,
			    user.username);
			gfarm_user_info_free(&user);
		} else {
			gflog_debug(GFARM_MSG_1000979,
				"gfm_client_user_info_"
				"get_by_gsi_dn(%s) failed: %s",
				gsi_dn, gfarm_error_string(e));
		}
	}
	return (e);
}
#endif

static gfarm_error_t
gfm_client_connection0(struct gfp_cached_connection *cache_entry,
	struct gfm_connection **gfm_serverp, const char *source_ip,
	struct passwd *pwd, gfarm_error_t (*connect_op)(const char *, int,
	    const char *, struct gfm_client_connect_info **,
	    struct pollfd **, int *))
{
	/* FIXME timeout must be configurable */
#define GFM_CLIENT_CONNECT_TIMEOUT (1000 * 10)
	gfarm_error_t e;
	struct gfm_connection *gfm_server;
	int port, sock;
	const char *hostname, *user;
	struct gfarm_metadb_server *ms = NULL;
	struct pollfd *pfds = NULL;
	struct addrinfo *res = NULL;
	struct gfm_client_connect_info *cis = NULL;
	struct gfarm_filesystem *fs;
#ifndef __KERNEL__	/* not used */
	int i, nfd, r, nerr = 0;
	struct pollfd *pfd;
	struct gfm_client_connect_info *ci;
	struct timeval timeout;
#endif /* __KERNEL__ */

	hostname = gfp_cached_connection_hostname(cache_entry);
	port = gfp_cached_connection_port(cache_entry);
	user = gfp_cached_connection_username(cache_entry);
#ifndef __KERNEL__	/* connect_op */
	/* connect_op is
	 *   gfm_client_connect_single or
	 *   gfm_client_connect_multiple
	 */
	if ((e = connect_op(hostname, port, source_ip, &cis, &pfds, &nfd))
	    != GFARM_ERR_NO_ERROR)
		return (e);
	gettimeofday(&timeout, NULL);
	timeout.tv_sec += GFM_CLIENT_CONNECT_TIMEOUT;
	sock = -1;
	e = GFARM_ERR_NO_ERROR;
	do {
		errno = 0;
		r = poll(pfds, nfd, 1000);
		if (r == -1) {
			e = gfarm_errno_to_error(errno);
			gflog_debug(GFARM_MSG_UNFIXED,
			    "poll: %s",
			    gfarm_error_string(e));
			break;
		}
		if (r == 0) {
			e = GFARM_ERR_CONNECTION_REFUSED;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "poll: %s",
			    gfarm_error_string(e));
			break;
		}
		for (i = 0; i < nfd; ++i) {
			ci = &cis[i];
			pfd = ci->pfd;
			if ((pfd->revents & (POLLERR|POLLHUP|POLLNVAL)) != 0) {
				close(pfd->fd);
				pfd->fd = -1;
				if (++nerr < nfd)
					continue;
				e = gfarm_errno_to_error(errno);
				if (e == GFARM_ERR_NO_ERROR)
					e = GFARM_ERR_CONNECTION_REFUSED;
				break;
			} else if ((pfd->revents & POLLIN) != 0) {
				sock = pfd->fd;
				ms = ci->ms;
				res = ci->res_ai;
				break;
			}
		}
	} while (sock == -1 && e == GFARM_ERR_NO_ERROR &&
	    !gfarm_timeval_is_expired(&timeout));

	for (i = 0; i < nfd; ++i) {
		ci = &cis[i];
		pfd = ci->pfd;
		if (sock == -1 || pfd->fd != sock) {
			if (pfd->fd >= 0)
				close(pfd->fd);
			gfarm_freeaddrinfo(ci->res_ai);
		}
	}
	if (e != GFARM_ERR_NO_ERROR)
		goto end;
#else
	{
		int err;
		if ((err = gfsk_gfmd_connect(hostname, port, source_ip,
				user, &sock)) != 0){
			return (gfarm_errno_to_error(err > 0 ? err : -err));
		}
	}
#endif
	GFARM_MALLOC(gfm_server);
	if (gfm_server == NULL) {
		close(sock);
		e = GFARM_ERR_NO_MEMORY;
		gflog_warning(GFARM_MSG_1001097,
			"allocation of 'gfm_server' failed: %s",
			gfarm_error_string(e));
		goto end;
	}
	/* add filesystem dynamically for Distributed MDS */
	fs = gfarm_filesystem_get(hostname, port);
	if (fs == NULL && (e = gfarm_filesystem_add(hostname, port, &fs))
	    != GFARM_ERR_NO_ERROR) {
		free(gfm_server);
		close(sock);
		gflog_warning(GFARM_MSG_UNFIXED,
		    "add filesystem failed: %s",
		    gfarm_error_string(e));
		goto end;
	}
	e = gfp_xdr_new_client_socket(sock, &gfm_server->conn);
	if (e != GFARM_ERR_NO_ERROR) {
		free(gfm_server);
		close(sock);
		gflog_warning(GFARM_MSG_1001098,
			"creation of new socket failed: %s",
			gfarm_error_string(e));
		goto end;
	}
#ifndef __KERNEL__	/* gfarm_auth_request */
	/* XXX We should explicitly pass the original global username too. */
	e = gfarm_auth_request(gfm_server->conn,
	    GFM_SERVICE_TAG, res->ai_canonname,
	    res->ai_addr, gfarm_get_auth_id_type(), user,
	    &gfm_server->auth_method, pwd);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_free(gfm_server->conn);
		free(gfm_server);
		gflog_debug(GFARM_MSG_1001099,
			"auth request failed: %s",
			gfarm_error_string(e));
		goto end;
	}
#ifdef HAVE_GSI
	/*
	 * In GSI authentication, small packets are sent frequently,
	 * which requires TCP_NODELAY for reasonable performance.
	 */
	if (gfm_server->auth_method == GFARM_AUTH_METHOD_GSI) {
		gfarm_error_t e1 = gfarm_sockopt_set_option(
		    gfp_xdr_fd(gfm_server->conn), "tcp_nodelay");

		if (e1 == GFARM_ERR_NO_ERROR)
			gflog_debug(GFARM_MSG_1003371, "tcp_nodelay is "
			    "specified for performance in GSI");
		else
			gflog_debug(GFARM_MSG_1003372, "tcp_nodelay is "
			    "specified, but fails: %s", gfarm_error_string(e1));
	}
#endif
#endif /* __KERNEL__ */
	gfm_server->cache_entry = cache_entry;
	gfp_cached_connection_set_data(cache_entry, gfm_server);
	gfm_server->pid = 0;
	gfm_server->real_server = ms != NULL ? ms :
		gfarm_filesystem_get_metadb_server_first(fs);
	gfm_server->failover_count = gfarm_filesystem_failover_count(fs);
	*gfm_serverp = gfm_server;
end:
	if (res)
		gfarm_freeaddrinfo(res);
	free(cis);
	free(pfds);
	return (e);
}
/*
 * gfm_client_connection_acquire - create or lookup a cached connection
 */
gfarm_error_t
gfm_client_connection_acquire(const char *hostname, int port,
	const char *user, struct gfm_connection **gfm_serverp)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;
	int created;
	unsigned int sleep_interval = 1;	/* 1 sec */
	unsigned int sleep_max_interval = 512;	/* about 8.5 min */
	struct timeval expiration_time;

retry:
	e = gfp_cached_connection_acquire(&staticp->server_cache,
	    hostname, port, user, &cache_entry, &created);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001100,
			"acquirement of cached connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!created) {
		*gfm_serverp = gfp_cached_connection_get_data(cache_entry);
		if (!*gfm_serverp) {
			gflog_warning(GFARM_MSG_UNFIXED,
				"gfm_client_connection_acquire:"
				"gfm_client_connection_acquire NULL");
			gfp_cached_or_uncached_connection_free(
				&staticp->server_cache, cache_entry);
			gfarm_nanosleep(10 * 1000 * 1000);
			goto retry;
		}
		return (GFARM_ERR_NO_ERROR);
	}
	e = gfm_client_connection0(cache_entry, gfm_serverp, NULL, NULL,
	    gfm_client_connect_multiple);
	gettimeofday(&expiration_time, NULL);
	expiration_time.tv_sec += gfarm_ctxp->gfmd_reconnection_timeout;
	while (IS_CONNECTION_ERROR(e) &&
	       !gfarm_timeval_is_expired(&expiration_time)) {
		gflog_warning(GFARM_MSG_1000058,
		    "connecting to gfmd at %s:%d failed, "
		    "sleep %d sec: %s", hostname, port, sleep_interval,
		    gfarm_error_string(e));
		sleep(sleep_interval);
		e = gfm_client_connection0(cache_entry, gfm_serverp, NULL,
		    NULL, gfm_client_connect_multiple);
		if (sleep_interval < sleep_max_interval)
			sleep_interval *= 2;
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1000059,
		    "cannot connect to gfmd at %s:%d, give up: %s",
		    hostname, port, gfarm_error_string(e));
		gfp_cached_connection_purge_from_cache(&staticp->server_cache,
		    cache_entry);
		gfp_uncached_connection_dispose(cache_entry);
	}
	return (e);
}

gfarm_error_t
gfm_client_connection_addref(struct gfm_connection *gfm_server)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;
	int created;

	e = gfp_cached_connection_acquire(&staticp->server_cache,
	    gfm_client_hostname(gfm_server),
	    gfm_client_port(gfm_server),
	    gfm_client_username(gfm_server),
	    &cache_entry, &created);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1003264,
			"addref of cached gfm connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!created) {
		assert(gfp_cached_connection_get_data(cache_entry)
		    == gfm_server);
		return (GFARM_ERR_NO_ERROR);
	}
	gflog_fatal(GFARM_MSG_1003265, "corrupted gfm connection cache");

	return (GFARM_ERR_UNKNOWN);
}

gfarm_error_t
gfm_client_connection_and_process_acquire(const char *hostname, int port,
	const char *user, struct gfm_connection **gfm_serverp)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server;
	int i, sleep_interval = 1;

	for (i = 0; i < CONNERR_RETRY_COUNT; ++i) {
		e = gfm_client_connection_acquire(hostname, port,
		    user, &gfm_server);

		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001101,
			    "acquirement of client connection failed: %s",
			    gfarm_error_string(e));
			break;
		}

		if (gfm_server->pid != 0)
			break;
		gfarm_auth_random(gfm_server->pid_key,
		    GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET);
		/*
		 * XXX FIXME
		 * should use COMPOUND request to reduce number of roundtrip
		 */
		e = gfm_client_process_alloc(gfm_server,
		    GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET,
		    gfm_server->pid_key,
		    GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET,
		    &gfm_server->pid);
		if (e == GFARM_ERR_NO_ERROR)
			break;

		gflog_error(GFARM_MSG_1000060,
		    "failed to allocate gfarm PID: %s",
		    gfarm_error_string(e));

		gfm_client_connection_free(gfm_server);
		if (IS_CONNECTION_ERROR(e) == 0)
			break;

		/* possibly gfmd failover or temporary error */
		sleep(sleep_interval);
		gflog_debug(GFARM_MSG_UNFIXED,
		    "retry to connect");
		sleep_interval *= 2;
	}

	if (e == GFARM_ERR_NO_ERROR) {
#ifdef HAVE_GSI
		/* obtain global username */
		if (GFARM_IS_AUTH_GSI(gfm_server->auth_method)) {
			e = gfarm_set_global_user_by_gsi(gfm_server);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_error(GFARM_MSG_1003450,
				    "cannot set global username: %s",
				    gfarm_error_string(e));
				gfm_client_connection_free(gfm_server);
				return (e);
			}
		}
#endif
		*gfm_serverp = gfm_server;
	}
	return (e);
}

#ifndef __KERNEL__	/* server only */

/*
 * gfm_client_connect - create an uncached connection
 */
gfarm_error_t
gfm_client_connect(const char *hostname, int port, const char *user,
	struct gfm_connection **gfm_serverp, const char *source_ip)
{
	return (gfm_client_connect_with_seteuid(hostname, port, user,
	    gfm_serverp, source_ip, NULL, 1));
}

/*
 * create an uncached connection. if sharedsecret is used
 * and pwd is passed, seteuid is called in gfarm_auth_shared_key_get()
 */
gfarm_error_t
gfm_client_connect_with_seteuid(const char *hostname, int port,
	const char *user, struct gfm_connection **gfm_serverp,
	const char *source_ip, struct passwd *pwd, int multicast)
{
	gfarm_error_t e;
	struct gfp_cached_connection *cache_entry;

	e = gfp_uncached_connection_new(hostname, port, user, &cache_entry);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001102,
			"creation of uncached connection failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	e = gfm_client_connection0(cache_entry, gfm_serverp, source_ip, pwd,
	    multicast ?
	    gfm_client_connect_multiple : gfm_client_connect_single);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001103,
			"gfm_client_connection0(%s)(%d) failed: %s",
			hostname, port,
			gfarm_error_string(e));
		gfp_uncached_connection_dispose(cache_entry);
	}
	return (e);
}
#endif /* __KERNEL__ */

static gfarm_error_t
gfm_client_connection_dispose(void *connection_data)
{
	struct gfm_connection *gfm_server = connection_data;
	gfarm_error_t e = GFARM_ERR_NO_ERROR;

	/*
	 * gfm_server->conn may be NULL, if this function is called
	 * from gfm_client_connection_convert_to_xdr()
	 */
	if (gfm_server->conn != NULL)
		e = gfp_xdr_free(gfm_server->conn);

	gfp_uncached_connection_dispose(gfm_server->cache_entry);
	free(gfm_server);
	return (e);
}

/*
 * gfm_client_connection_free() can be used for both
 * an uncached connection which was created by gfm_client_connect(), and
 * a cached connection which was created by gfm_client_connection_acquire().
 * The connection will be immediately closed in the former uncached case.
 *
 */
void
gfm_client_connection_free(struct gfm_connection *gfm_server)
{
	gfp_cached_or_uncached_connection_free(&staticp->server_cache,
	    gfm_server->cache_entry);
}

/*
 * NOTE: this function frees struct gfm_connection
 */
struct gfp_xdr *
gfm_client_connection_convert_to_xdr(struct gfm_connection *gfm_server)
{
	struct gfp_xdr *conn = gfm_server->conn;

	gfm_server->conn = NULL;
	gfm_client_connection_free(gfm_server);
	return (conn);
}

void
gfm_client_terminate(void)
{
	gfp_cached_connection_terminate(&staticp->server_cache);
}
void
gfm_client_connection_unlock(struct gfm_connection *gfm_server)
{
	gfp_connection_unlock(gfm_server->cache_entry);
}
void
gfm_client_connection_lock(struct gfm_connection *gfm_server)
{
	gfp_connection_lock(gfm_server->cache_entry);
}

static void
check_connection_or_purge(struct gfm_connection *gfm_server,
	gfarm_error_t e)
{
	if (IS_CONNECTION_ERROR(e)) {
		gfm_client_purge_from_cache(gfm_server);
		/*
		 * failover-detected flag must always be incremented
		 * at purging gfm_connection to replace gfm_connection
		 * in gfarm_url_parse_metadb(),
		 * gfs_client_connection_and_process_acquire().
		 */
		gfarm_filesystem_set_failover_detected(
		    gfarm_filesystem_get_by_connection(gfm_server), 1);
	}
}

static gfarm_error_t
gfm_client_xdr_send(struct gfm_connection *gfm_server, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vsend(gfm_server->conn, &format, &ap);
	va_end(ap);

	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfp_xdr_send: %s",
		    gfarm_error_string(e));
	} else if (*format != '\0') {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "invalid format character: %c(%x)",
		    *format, *format);
		e = GFARM_ERRMSG_GFP_XDR_SEND_INVALID_FORMAT_CHARACTER;
	}
	return (e);
}

static gfarm_error_t
gfm_client_xdr_recv(struct gfm_connection *gfm_server,
	size_t *sizep, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int eof;

	va_start(ap, format);
	e = gfp_xdr_vrecv_sized_x(gfm_server->conn, 0, 1, sizep, &eof,
		&format, &ap);
	va_end(ap);

	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfp_xdr_vrecv_sized_x: %s",
		    gfarm_error_string(e));
	} else if (eof) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfp_xdr_vrecv_sized_x: unexpected EOF");
		e = GFARM_ERR_UNEXPECTED_EOF;
	} else if (*format != '\0') {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "invalid format character: %c(%x)", *format, *format);
		e = GFARM_ERRMSG_GFP_XDR_RECV_INVALID_FORMAT_CHARACTER;
	}
	return (e);
}

static gfarm_error_t
gfm_client_vrpc_raw_request_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record **xidrp, int *size_posp,
	int command, const char **formatp, va_list *app)
{
	gfm_client_connection_used(gfm_server);

	return (gfp_xdr_vrpc_raw_request_begin(gfm_server->conn,
	    xidrp, size_posp, command, formatp, app));

}

static gfarm_error_t
gfm_client_rpc_raw_request_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record **xidrp, int *size_posp,
	int command, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfm_client_vrpc_raw_request_begin(gfm_server, xidrp, size_posp,
	    command, &format, &ap);
	va_end(ap);

	return (e);
}

gfarm_error_t
gfm_client_context_alloc(struct gfm_connection *gfm_server,
	struct gfp_xdr_context **ctxp)
{
	return (gfp_xdr_context_alloc(gfm_server->conn, ctxp));
}

void
gfm_client_context_free(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	gfp_xdr_context_free(gfm_server->conn, ctx);
}

void
gfm_client_context_free_until(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, struct gfp_xdr_xid_record *xidr)
{
	gfp_xdr_context_free_until(gfm_server->conn, ctx, xidr);
}

struct gfp_xdr_xid_record *
gfm_client_context_get_pos(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfp_xdr_context_get_pos(gfm_server->conn, ctx));
}

static gfarm_error_t
gfm_client_vrpc_request_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int *size_posp,
	int command, const char **formatp, va_list *app)
{
	gfm_client_connection_used(gfm_server);

	return (gfp_xdr_vrpc_request_begin(gfm_server->conn, ctx, size_posp,
	    command, formatp, app));
}

static gfarm_error_t
gfm_client_rpc_request_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int *size_posp,
	int command, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfm_client_vrpc_request_begin(gfm_server, ctx, size_posp,
	    command, &format, &ap);
	va_end(ap);

	return (e);
}

static void
gfm_client_rpc_request_epilogue(struct gfm_connection *gfm_server,
	int command, gfarm_error_t e)
{
	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001104,
		    "gfp_xdr_vrpc_request(%d) failed: %s", command,
		    gfarm_error_string(e));
	}
}

static gfarm_error_t
gfm_client_rpc_raw_request_end(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, int size_pos, int command)
{
	gfarm_error_t e;

	e = gfp_xdr_rpc_raw_request_end(gfm_server->conn, xidr, size_pos);
	gfm_client_rpc_request_epilogue(gfm_server, command, e);
	return (e);
}

static gfarm_error_t
gfm_client_rpc_request_end(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int size_pos, int command)
{
	gfarm_error_t e;

	e = gfp_xdr_rpc_request_end(gfm_server->conn, ctx, size_pos);
	gfm_client_rpc_request_epilogue(gfm_server, command, e);
	return (e);
}

gfarm_error_t
gfm_client_rpc_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int command, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int size_pos;

	va_start(ap, format);
	e = gfp_xdr_vrpc_request_begin(gfm_server->conn, ctx, &size_pos,
	    command, &format, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	return (gfm_client_rpc_request_end(gfm_server, ctx, size_pos,
	    command));
}

static gfarm_error_t
gfm_client_rpc_result_prologue(struct gfm_connection *gfm_server)
{
	gfarm_error_t e;

	gfm_client_connection_used(gfm_server);

	e = gfp_xdr_flush(gfm_server->conn);

	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1001105,
		    "gfp_xdr_flush() failed: %s", gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfm_client_rpc_result_begin_epilogue(struct gfm_connection *gfm_server,
	gfarm_error_t e, gfarm_int32_t errcode)
{
	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001106,
		    "gfp_xdr_vrpc*_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (errcode != 0) {
		/*
		 * We just use gfarm_error_t as the errcode,
		 * Note that GFARM_ERR_NO_ERROR == 0.
		 */
#if 0		/* verbose message */
		gflog_debug(GFARM_MSG_1001107,
			"gfp_xdr_vrpc_result() failed: errcode=%d",
			errcode);
#endif
		return (errcode);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfm_client_vrpc_raw_result_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, size_t *sizep,
	const char **formatp, va_list *app)
{
	gfarm_error_t e;
	gfarm_int32_t errcode = 0;

	e = gfm_client_rpc_result_prologue(gfm_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = gfp_xdr_vrpc_raw_result_begin(gfm_server->conn, 0, 1, xidr, sizep,
	    &errcode, formatp, app);
	if (e == GFARM_ERR_NO_ERROR &&
	    errcode == GFARM_ERR_RPC_REQUEST_IGNORED)
		errcode = GFARM_ERR_NO_ERROR;

	return (gfm_client_rpc_result_begin_epilogue(gfm_server, e, errcode));
}

static gfarm_error_t
gfm_client_rpc_raw_result_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, size_t *sizep,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfm_client_vrpc_raw_result_begin(gfm_server, xidr, sizep,
	    &format, &ap);
	va_end(ap);

	return (e);
}

static gfarm_error_t
gfm_client_vrpc_result_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, size_t *sizep,
	const char **formatp, va_list *app)
{
	gfarm_error_t e;
	gfarm_int32_t errcode = 0;

	e = gfm_client_rpc_result_prologue(gfm_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = gfp_xdr_vrpc_result_begin(gfm_server->conn, 0, 1, ctx, sizep,
	    &errcode, formatp, app);
	if (e == GFARM_ERR_NO_ERROR &&
	    errcode == GFARM_ERR_RPC_REQUEST_IGNORED)
		errcode = GFARM_ERR_NO_ERROR;

	return (gfm_client_rpc_result_begin_epilogue(gfm_server, e, errcode));
}

static gfarm_error_t
gfm_client_rpc_result_begin(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, size_t *sizep,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfm_client_vrpc_result_begin(gfm_server, ctx, sizep, &format, &ap);
	va_end(ap);

	return (e);
}

static gfarm_error_t
gfm_client_rpc_result_end_epilogue(struct gfm_connection *gfm_server,
	gfarm_error_t e)
{
	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfp_xdr_vrpc*_result_end() failed: %s",
		    gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfm_client_rpc_raw_result_end(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, size_t size)
{
	gfarm_error_t e;

	e = gfp_xdr_rpc_raw_result_end(gfm_server->conn, 0, xidr, size);
	return (gfm_client_rpc_result_end_epilogue(gfm_server, e));
}

static gfarm_error_t
gfm_client_rpc_result_end(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, size_t size)
{
	gfarm_error_t e;

	e = gfp_xdr_rpc_result_end(gfm_server->conn, 0, ctx, size);
	return (gfm_client_rpc_result_end_epilogue(gfm_server, e));
}

gfarm_error_t
gfm_client_rpc_raw_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	size_t size;

	va_start(ap, format);
	e = gfm_client_vrpc_raw_result_begin(gfm_server, xidr, &size,
	    &format, &ap);
	va_end(ap);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	return (gfm_client_rpc_raw_result_end(gfm_server, xidr, size));
}

gfarm_error_t
gfm_client_rpc_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	size_t size;

	va_start(ap, format);
	e = gfm_client_vrpc_result_begin(gfm_server, ctx, &size, &format, &ap);
	va_end(ap);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	return (gfm_client_rpc_result_end(gfm_server, ctx, size));
}

static gfarm_error_t
gfm_client_rpc_request_and_result_begin(
	struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record **xidrp, size_t *sizep,
	int command, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int size_pos;

	va_start(ap, format);
	e = gfm_client_vrpc_raw_request_begin(gfm_server, xidrp, &size_pos,
	    command, &format, &ap);
	
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_begin() failed: %s",
		    gfarm_error_string(e));
		va_end(ap);
		return (e);
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, *xidrp, size_pos,
	    command)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		va_end(ap);
		return (e); /* XXX xid memory leak */
	}
	if (*format != '/' && *format != '\0') {
#if 1
		gflog_fatal(GFARM_MSG_UNFIXED, "%s",
		    gfarm_error_string(GFARM_ERRMSG_GFP_XDR_VRPC_MISSING_RESULT_IN_FORMAT_STRING));
#endif
		return (GFARM_ERRMSG_GFP_XDR_VRPC_MISSING_RESULT_IN_FORMAT_STRING);
	}
	if (*format == '/')
		++format;

	e = gfm_client_vrpc_raw_result_begin(gfm_server, *xidrp, sizep,
	    &format, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1001118,
			"get_client_rpc_raw_result_begin() failed: %s",
			gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfm_client_rpc(struct gfm_connection *gfm_server, int command,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int errcode;

	gfm_client_connection_used(gfm_server);
	gfm_client_connection_lock(gfm_server);

	va_start(ap, format);
	e = gfp_xdr_vrpc(gfm_server->conn, 0, 1,
	    command, &errcode, &format, &ap);
	va_end(ap);

	gfm_client_connection_unlock(gfm_server);
	check_connection_or_purge(gfm_server, e);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001108,
			"gfp_xdr_vrpc(%d) failed: %s",
		command,
			gfarm_error_string(e));
		return (e);
	}
	if (errcode == GFARM_ERR_RPC_REQUEST_IGNORED) {
		/* this is OK */
	} else if (errcode != 0) {
		/*
		 * We just use gfarm_error_t as the errcode,
		 * Note that GFARM_ERR_NO_ERROR == 0.
		 */
		gflog_debug(GFARM_MSG_1001109,
			"gfp_xdr_vrpc() failed: errcode=%d",
			errcode);
		return (errcode);
	}
	return (GFARM_ERR_NO_ERROR);
}

/*
 * host/user/group metadata
 */

/* this interface is exported for a use from a private extension */
gfarm_error_t
gfm_client_get_nhosts(struct gfm_connection *gfm_server,
	size_t *sizep, int nhosts, struct gfarm_host_info *hosts)
{
	gfarm_error_t e;
	int i;
	gfarm_int32_t naliases;

	for (i = 0; i < nhosts; i++) {
		e = gfm_client_xdr_recv(gfm_server, sizep, "ssiiii",
		    &hosts[i].hostname, &hosts[i].architecture,
		    &hosts[i].ncpu, &hosts[i].port, &hosts[i].flags,
		    &naliases);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
			    "gfm_client_xdr_recv() failed: %s",
			    gfarm_error_string(e));
			return (e);
		}
		/* XXX FIXME */
		hosts[i].nhostaliases = 0;
		hosts[i].hostaliases = NULL;
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfm_client_host_info_get_n(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, size_t size, int nhosts,
	int *nhostsp, struct gfarm_host_info **hostsp, const char *diag)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR, e2;
	struct gfarm_host_info *hosts;

	GFARM_MALLOC_ARRAY(hosts, nhosts);
	if (hosts == NULL) { /* XXX this breaks gfm protocol */
		gflog_debug(GFARM_MSG_1002208,
		    "host_info allocation for %d hosts: no memory", nhosts);
		e = GFARM_ERR_NO_MEMORY;
	} else if ((e = gfm_client_get_nhosts(gfm_server, &size, nhosts,
	    hosts)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001114,
		    "gfm_client_get_nhosts() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	e2 = gfm_client_rpc_raw_result_end(gfm_server, xidr, size);
	if (e != GFARM_ERR_NO_ERROR) {
		return (e);
	} else if (e2 != GFARM_ERR_NO_ERROR ) {
		/* XXX hosts memory leak */
		return (e2);
	}
	*nhostsp = nhosts;
	*hostsp = hosts;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_host_info_get_all(struct gfm_connection *gfm_server,
	int *nhostsp, struct gfarm_host_info **hostsp)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	gfarm_int32_t nhosts;
	static const char diag[] = "gfm_client_host_info_get_all";

	gfm_client_connection_lock(gfm_server);
	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_HOST_INFO_GET_ALL, "/i",
	    &nhosts)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001112,
			"gfm_client_rpc() failed: %s",
			gfarm_error_string(e));
	} else
		e = gfm_client_host_info_get_n(gfm_server, xidr, size,
		    nhosts, nhostsp, hostsp, diag);
	gfm_client_connection_unlock(gfm_server);
	return (e);
}

gfarm_error_t
gfm_client_host_info_get_by_architecture(struct gfm_connection *gfm_server,
	const char *architecture,
	int *nhostsp, struct gfarm_host_info **hostsp)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	int nhosts;
	static const char diag[] = "gfm_client_host_info_get_by_architecture";

	gfm_client_connection_lock(gfm_server);
	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size,
	    GFM_PROTO_HOST_INFO_GET_BY_ARCHITECTURE, "s/i", architecture,
	    &nhosts)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001115,
			"gfm_client_rpc() failed: %s",
			gfarm_error_string(e));
	} else 
		e = gfm_client_host_info_get_n(gfm_server, xidr, size,
			    nhosts, nhostsp, hostsp, diag);
	return (e);
}

static gfarm_error_t
gfm_client_host_info_get_by_names_common(struct gfm_connection *gfm_server,
	int op, int nhosts, const char **names,
	gfarm_error_t *errors, struct gfarm_host_info *hosts)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, size_pos;
	size_t size;
	gfarm_int32_t errcode;

	gfm_client_connection_lock(gfm_server);
	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    op, "i", nhosts)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001116,
		    "gfm_client_rpc_raw_request_begin() failed: %s",
		    gfarm_error_string(e));
		goto out;
	}
	for (i = 0; i < nhosts; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", names[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001117,
			    "sending hostname (%s) failed: %s",
			    names[i], gfarm_error_string(e));
			goto out; /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos, op)
	    ) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		goto out; /* XXX xid memory leak */
	}

	if ((e = gfm_client_rpc_raw_result_begin(gfm_server, xidr, &size, ""))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001118,
			"get_client_rpc_raw_result_begin() failed: %s",
			gfarm_error_string(e));
		goto out;
	}
	for (i = 0; i < nhosts; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "i", &errcode);
		if (e != GFARM_ERR_NO_ERROR)
			goto out; /* XXX xid and hosts memory leak */
		else if (errcode != GFARM_ERR_NO_ERROR)
			errors[i] = errcode;
		else if ((e = gfm_client_get_nhosts(gfm_server,
		    &size, 1, &hosts[i])) != GFARM_ERR_NO_ERROR)
			goto out; /* XXX xid hosts memory leak */
		else
			errors[i] = GFARM_ERR_NO_ERROR;
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		goto out; /* XXX hosts memory leak */
	}
out:
	gfm_client_connection_unlock(gfm_server);
	return (e);
}

gfarm_error_t
gfm_client_host_info_get_by_names(struct gfm_connection *gfm_server,
	int nhosts, const char **names,
	gfarm_error_t *errors, struct gfarm_host_info *hosts)
{
	return (gfm_client_host_info_get_by_names_common(
	    gfm_server, GFM_PROTO_HOST_INFO_GET_BY_NAMES,
	    nhosts, names, errors, hosts));
}

gfarm_error_t
gfm_client_host_info_get_by_namealiases(struct gfm_connection *gfm_server,
	int nhosts, const char **names,
	gfarm_error_t *errors, struct gfarm_host_info *hosts)
{
	return (gfm_client_host_info_get_by_names_common(
	    gfm_server, GFM_PROTO_HOST_INFO_GET_BY_NAMEALIASES,
	    nhosts, names, errors, hosts));
}

static gfarm_error_t
gfm_client_host_info_send_common(struct gfm_connection *gfm_server,
	int op, const struct gfarm_host_info *host)
{
	return (gfm_client_rpc(gfm_server, op, "ssiii/",
	    host->hostname, host->architecture,
	    host->ncpu, host->port, host->flags));
}

gfarm_error_t
gfm_client_host_info_set(struct gfm_connection *gfm_server,
	const struct gfarm_host_info *host)
{
	return (gfm_client_host_info_send_common(gfm_server,
	    GFM_PROTO_HOST_INFO_SET, host));
}

gfarm_error_t
gfm_client_host_info_modify(struct gfm_connection *gfm_server,
	const struct gfarm_host_info *host)
{
	return (gfm_client_host_info_send_common(gfm_server,
	    GFM_PROTO_HOST_INFO_MODIFY, host));
}

gfarm_error_t
gfm_client_host_info_remove(struct gfm_connection *gfm_server,
	const char *hostname)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_HOST_INFO_REMOVE, "s/", hostname));
}

/* fsngroup */
gfarm_error_t
gfm_client_fsngroup_get_all(struct gfm_connection *gfm_server,
	size_t *np, struct gfarm_fsngroup_info **infos)
{
	gfarm_error_t e = GFARM_ERR_UNKNOWN, e2;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	gfarm_int32_t nhosts;
	size_t n = 0;
	size_t i;
	struct gfarm_fsngroup_info *ret = NULL;
	static const char diag[] = "gfm_client_fsngroup_get_all";

	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_FSNGROUP_GET_ALL, "/i", &nhosts))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
			"%s: gfm_client_rpc() failed: %s",
			diag, gfarm_error_string(e));
		goto bailout;
	}

	n = (size_t)nhosts;
	if (nhosts > 0) {
		GFARM_MALLOC_ARRAY(ret, (size_t)nhosts);
		if (ret == NULL) {
			gflog_debug(GFARM_MSG_UNFIXED,
				"%s: can't allocate fsngroup_info(s).", diag);
			e = GFARM_ERR_NO_MEMORY;
			goto rpc_cleanup;
		}

		for (i = 0; i < n; i++) {
			e = gfm_client_xdr_recv(gfm_server, &size, "ss",
				&(ret[i].hostname), &(ret[i].fsngroupname));
			if (e != GFARM_ERR_NO_ERROR) {
				if (e != GFARM_ERR_NO_ERROR)
					gflog_debug(GFARM_MSG_UNFIXED,
						"%s: gfm_client_xdr_recv() "
						"failed: %s",
						diag, gfarm_error_string(e));
				n = i;
				break;
			}
		}
	}

rpc_cleanup:
	e2 = gfm_client_rpc_raw_result_end(gfm_server, xidr, size);
	if (e == GFARM_ERR_NO_ERROR)
		e = e2;

bailout:
	if (e != GFARM_ERR_NO_ERROR) {
		np = NULL;
		infos = NULL;
	}
	if (np != NULL)
		*np = n;
	if (infos != NULL) {
		*infos = ret;
	} else {
		for (i = 0; i < n; i++) {
			free(ret[i].hostname);
			free(ret[i].fsngroupname);
		}
		free(ret);
	}
	return (e);
}

gfarm_error_t
gfm_client_fsngroup_get_by_hostname(struct gfm_connection *gfm_server,
	const char *hostname, char **fsngroupnamep)
{
	gfarm_error_t e = GFARM_ERR_UNKNOWN;
	static const char diag[] = "gfm_client_fsngroup_get_by_hostname";
	char *ret = NULL;

	if ((e = gfm_client_rpc(gfm_server,
			GFM_PROTO_FSNGROUP_GET_BY_HOSTNAME, "s/s",
			hostname, &ret)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
			"%s: gfm_client_rpc() failed: %s",
			diag, gfarm_error_string(e));
	}

	if (fsngroupnamep != NULL)
		*fsngroupnamep = ret;
	else
		free(ret);

	return (e);
}

gfarm_error_t
gfm_client_fsngroup_modify(struct gfm_connection *gfm_server,
	struct gfarm_fsngroup_info *info)
{
	gfarm_error_t e = GFARM_ERR_UNKNOWN;
	static const char diag[] = "gfm_client_fsngroup_modify";

	if ((e = gfm_client_rpc(gfm_server,
			GFM_PROTO_FSNGROUP_MODIFY, "ss/",
			info->hostname,
			info->fsngroupname)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
			"%s: gfm_client_rpc() failed: %s",
			diag, gfarm_error_string(e));
	}

	return (e);
}

static gfarm_error_t
get_nusers(struct gfm_connection *gfm_server, size_t *sizep,
	int nusers, struct gfarm_user_info *users)
{
	gfarm_error_t e;
	int i;

	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_recv(gfm_server, sizep, "ssss",
		    &users[i].username, &users[i].realname,
		    &users[i].homedir, &users[i].gsi_dn);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001119,
				"receiving users failed: %s",
				gfarm_error_string(e));
			return (e);
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_user_info_get_all(struct gfm_connection *gfm_server,
	int *nusersp, struct gfarm_user_info **usersp)
{
	gfarm_error_t e, e2;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	int nusers;
	struct gfarm_user_info *users;

	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_USER_INFO_GET_ALL,
	    "/i", &nusers)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001121,
			"gfm_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	GFARM_MALLOC_ARRAY(users, nusers);
	if (users == NULL) { /* XXX this breaks gfm protocol */
		gflog_debug(GFARM_MSG_1001122,
			"allocation of array 'users' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		e = GFARM_ERR_NO_MEMORY;
	} else if ((e = get_nusers(gfm_server, &size, nusers, users))
		!= GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001123,
			"get_nusers() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	e2 = gfm_client_rpc_raw_result_end(gfm_server, xidr, size);
	if (e != GFARM_ERR_NO_ERROR) {
		return (e);
	} else if (e2 != GFARM_ERR_NO_ERROR ) {
		/* XXX users memory leak */
		return (e2);
	}
	*nusersp = nusers;
	*usersp = users;
	return (e);
}

gfarm_error_t
gfm_client_user_info_get_by_names(struct gfm_connection *gfm_server,
	int nusers, const char **names,
	gfarm_error_t *errors, struct gfarm_user_info *users)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, size_pos;
	size_t size;
	gfarm_int32_t errcode;

	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    GFM_PROTO_USER_INFO_GET_BY_NAMES, "i", nusers)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001124,
		    "gfm_client_rpc_raw_request_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", names[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001125,
			    "sending username (%s) failed: %s",
			    names[i], gfarm_error_string(e));
			return (e); /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos,
	    GFM_PROTO_USER_INFO_GET_BY_NAMES)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}

	if ((e = gfm_client_rpc_raw_result_begin(gfm_server, xidr, &size, ""))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001126,
		    "gfm_client_rpc_raw_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "i", &errcode);
		if (e != GFARM_ERR_NO_ERROR)
			errors[i] = e;
		else if (errcode != GFARM_ERR_NO_ERROR)
			errors[i] = errcode;
		else
			errors[i] = get_nusers(gfm_server, &size,
			    1, &users[i]);
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX users memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_user_info_get_by_gsi_dn(struct gfm_connection *gfm_server,
	const char *gsi_dn, struct gfarm_user_info *user)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_USER_INFO_GET_BY_GSI_DN, "s/ssss", gsi_dn,
	    &user->username, &user->realname, &user->homedir, &user->gsi_dn));
}

static gfarm_error_t
gfm_client_user_info_send_common(struct gfm_connection *gfm_server,
	int op, const struct gfarm_user_info *user)
{
	return (gfm_client_rpc(gfm_server,
	    op, "ssss/",
	    user->username, user->realname, user->homedir, user->gsi_dn));
}

gfarm_error_t
gfm_client_user_info_set(struct gfm_connection *gfm_server,
	const struct gfarm_user_info *user)
{
	return (gfm_client_user_info_send_common(gfm_server,
	    GFM_PROTO_USER_INFO_SET, user));
}

gfarm_error_t
gfm_client_user_info_modify(struct gfm_connection *gfm_server,
	const struct gfarm_user_info *user)
{
	return (gfm_client_user_info_send_common(gfm_server,
	    GFM_PROTO_USER_INFO_MODIFY, user));
}

gfarm_error_t
gfm_client_user_info_remove(struct gfm_connection *gfm_server,
	const char *username)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_USER_INFO_REMOVE, "s/", username));
}

static gfarm_error_t
get_ngroups(struct gfm_connection *gfm_server, size_t *sizep,
	int ngroups, struct gfarm_group_info *groups)
{
	gfarm_error_t e;
	int i, j;

	for (i = 0; i < ngroups; i++) {
		e = gfm_client_xdr_recv(gfm_server, sizep, "si",
		    &groups[i].groupname, &groups[i].nusers);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001127,
				"receiving groups response failed: %s",
				gfarm_error_string(e));
			return (e); /* XXX */
		}
		GFARM_MALLOC_ARRAY(groups[i].usernames, groups[i].nusers);
		if (groups[i].usernames == NULL) {
			gflog_debug(GFARM_MSG_1001129,
				"allocation of array 'groups' failed: %s",
				gfarm_error_string(GFARM_ERR_NO_MEMORY));
			return (GFARM_ERR_NO_MEMORY); /* XXX */
		}
		for (j = 0; j < groups[i].nusers; j++) {
			e = gfm_client_xdr_recv(gfm_server, sizep, "s",
			    &groups[i].usernames[j]);
			if (e != GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1001130,
					"receiving group response failed: %s",
					gfarm_error_string(e));
				return (e); /* XXX memory leak */
			}
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_group_info_get_all(struct gfm_connection *gfm_server,
	int *ngroupsp, struct gfarm_group_info **groupsp)
{
	gfarm_error_t e, e2;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	int ngroups;
	struct gfarm_group_info *groups;

	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_GROUP_INFO_GET_ALL,
	    "/i", &ngroups)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001134,
			"gfm_client_rpc() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	GFARM_MALLOC_ARRAY(groups, ngroups);
	if (groups == NULL) {
		gflog_debug(GFARM_MSG_1001135,
			"allocation of array 'groups' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		e = GFARM_ERR_NO_MEMORY;
	} else if ((e = get_ngroups(gfm_server, &size, ngroups, groups)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001136,
			"get_ngroups() failed: %s",
			gfarm_error_string(e));
	}
	e2 = gfm_client_rpc_raw_result_end(gfm_server, xidr, size);
	if (e != GFARM_ERR_NO_ERROR) {
		return (e);
	} else if (e2 != GFARM_ERR_NO_ERROR ) {
		/* XXX groups memory leak */
		return (e2);
	}
	*ngroupsp = ngroups;
	*groupsp = groups;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_group_info_get_by_names(struct gfm_connection *gfm_server,
	int ngroups, const char **group_names,
	gfarm_error_t *errors, struct gfarm_group_info *groups)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, size_pos;
	size_t size;
	gfarm_int32_t errcode;

	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    GFM_PROTO_GROUP_INFO_GET_BY_NAMES, "i", ngroups)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001137,
		    "gfm_client_rpc_raw_request_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < ngroups; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", group_names[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001138,
			    "sending groupname (%s) failed: %s",
			    group_names[i], gfarm_error_string(e));
			return (e); /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos,
	    GFM_PROTO_GROUP_INFO_GET_BY_NAMES)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}
	if ((e = gfm_client_rpc_raw_result_begin(gfm_server, xidr, &size, ""))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001139,
		    "gfm_client_rpc_raw_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < ngroups; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "i", &errcode);
		if (e != GFARM_ERR_NO_ERROR)
			errors[i] = e;
		else if (errcode != GFARM_ERR_NO_ERROR)
			errors[i] = errcode;
		else
			errors[i] = get_ngroups(gfm_server, &size,
			    1, &groups[i]);
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX groups memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfm_client_group_info_send_common(struct gfm_connection *gfm_server,
	int op, const struct gfarm_group_info *group)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, size_pos;

	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    op, "si", group->groupname, group->nusers))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001132,
		    "gfm_client_rpc_raw_request_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < group->nusers; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", group->usernames[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001133,
			    "sending username (%s) failed: %s",
			    group->usernames[i], gfarm_error_string(e));
			return (e); /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos, op)
	    ) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}

	return (gfm_client_rpc_raw_result(gfm_server, xidr, ""));
}

gfarm_error_t
gfm_client_group_info_set(struct gfm_connection *gfm_server,
	const struct gfarm_group_info *group)
{
	return (gfm_client_group_info_send_common(gfm_server,
	    GFM_PROTO_GROUP_INFO_SET, group));
}

gfarm_error_t
gfm_client_group_info_modify(struct gfm_connection *gfm_server,
	const struct gfarm_group_info *group)
{
	return (gfm_client_group_info_send_common(gfm_server,
	    GFM_PROTO_GROUP_INFO_MODIFY, group));
}

gfarm_error_t
gfm_client_group_info_remove(struct gfm_connection *gfm_server,
	const char *groupname)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_GROUP_INFO_REMOVE, "s/", groupname));
}

gfarm_error_t
gfm_client_group_info_users_op_common(struct gfm_connection *gfm_server,
	int op, const char *groupname,
	int nusers, const char **usernames, gfarm_error_t *errors)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, size_pos;
	size_t size;
	gfarm_int32_t errcode;

	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    op, "si", groupname, nusers)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001140,
			"gfm_client_rpc_request() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", usernames[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001141,
			    "sending username (%s) failed: %s",
			    usernames[i], gfarm_error_string(e));
			return (e); /* xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos, op)
	    ) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}
	if ((e = gfm_client_rpc_raw_result_begin(gfm_server, xidr, &size, ""))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001139,
		    "gfm_client_rpc_raw_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "i", &errcode);
		if (e != GFARM_ERR_NO_ERROR)
			errors[i] = e;
		else
			errors[i] = errcode;
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX users memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_group_info_add_users(struct gfm_connection *gfm_server,
	const char *groupname,
	int nusers, const char **usernames, gfarm_error_t *errors)
{
	return (gfm_client_group_info_users_op_common(gfm_server,
	    GFM_PROTO_GROUP_INFO_ADD_USERS, groupname, nusers, usernames,
	    errors));
}

gfarm_error_t
gfm_client_group_info_remove_users(struct gfm_connection *gfm_server,
	const char *groupname,
	int nusers, const char **usernames, gfarm_error_t *errors)
{
	return (gfm_client_group_info_users_op_common(gfm_server,
	    GFM_PROTO_GROUP_INFO_REMOVE_USERS, groupname, nusers, usernames,
	    errors));
}

gfarm_error_t
gfm_client_group_names_get_by_users(struct gfm_connection *gfm_server,
	int nusers, const char **usernames,
	gfarm_error_t *errors, struct gfarm_group_names *assignments)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	int i, j, size_pos;
	size_t size;
	gfarm_int32_t errcode;

	if ((e = gfm_client_rpc_raw_request_begin(gfm_server, &xidr, &size_pos,
	    GFM_PROTO_GROUP_NAMES_GET_BY_USERS, "i", nusers)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001143,
			"gfm_client_rpc_request() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", usernames[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001144,
			    "sending username (%s) failed: %s",
			    usernames[i], gfarm_error_string(e));
			return (e); /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_request_end(gfm_server, xidr, size_pos,
	    GFM_PROTO_GROUP_NAMES_GET_BY_USERS)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_raw_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}

	if ((e = gfm_client_rpc_raw_result_begin(gfm_server, xidr, &size, ""))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001145,
		    "gfm_client_rpc_raw_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < nusers; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "i", &errcode);
		if (e != GFARM_ERR_NO_ERROR)
			errors[i] = e;
		else if (errcode != GFARM_ERR_NO_ERROR)
			errors[i] = errcode;
		else if ((e = gfm_client_xdr_recv(gfm_server, &size, "i",
		    &assignments->ngroups)) == GFARM_ERR_NO_ERROR) {
			GFARM_MALLOC_ARRAY(assignments->groupnames,
			    assignments->ngroups);
			if (assignments->groupnames == NULL) {
				errors[i] = GFARM_ERR_NO_MEMORY;
			} else {
				for (j = 0; j < assignments->ngroups; j++) {
					errors[i] = gfm_client_xdr_recv(
					    gfm_server, &size, "s",
					    &assignments->groupnames[j]);
					if (errors[i] != GFARM_ERR_NO_ERROR)
						break;
				}
			}
		}
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX assignments[].groupnames memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

/*
 * gfs from client
 */

gfarm_error_t
gfm_client_compound_begin_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_COMPOUND_BEGIN, ""));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_COMPOUND_BEGIN, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_BEGIN>:%p:%d request: %s", gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_compound_begin_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_BEGIN>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_compound_end_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_COMPOUND_END,
	    ""));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_COMPOUND_END,
	    "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_END>:%p:%d request: %s", gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_compound_end_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_END>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_compound_on_error_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_error_t error)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_COMPOUND_ON_ERROR, "i", error));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_COMPOUND_ON_ERROR, "i", error);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_ON_ERROR>(%s):%p:%d request: %s", gfarm_error_string(error), gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_compound_on_error_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_COMPOUND_ON_ERROR>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_get_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_GET_FD, ""));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_GET_FD, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_GET_FD>:%p:%d request: %s", gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_get_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t *fdp)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, "i", fdp));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "i", fdp);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_GET_FD>:%p:%d result=>%d: %s", gfm_server->conn, xid, e == GFARM_ERR_NO_ERROR ? *fdp : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_put_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t fd)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_PUT_FD,
	    "i", fd));
}

gfarm_error_t
gfm_client_put_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_save_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_SAVE_FD, ""));
}

gfarm_error_t
gfm_client_save_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_restore_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_RESTORE_FD, ""));
}

gfarm_error_t
gfm_client_restore_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_create_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *name, gfarm_uint32_t flags, gfarm_uint32_t mode)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_CREATE,
	    "sii", name, flags, mode));
}

gfarm_error_t
gfm_client_create_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_ino_t *inump, gfarm_uint64_t *genp, gfarm_mode_t *modep)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "lli",
	    inump, genp, modep));
}

gfarm_error_t
gfm_client_open_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *name, size_t namelen, gfarm_uint32_t flags)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_OPEN,
	    "Si", name, namelen, flags));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_OPEN,
	    "Si", name, namelen, flags);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_OPEN>(%.*s, 0x%x):%p:%d request: %s", (int)namelen, name, flags, gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_open_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_ino_t *inump, gfarm_uint64_t *genp, gfarm_mode_t *modep)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, "lli",
	    inump, genp, modep));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "lli",
	    inump, genp, modep);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_OPEN>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_open_root_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_uint32_t flags)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_OPEN_ROOT, "i", flags));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_OPEN_ROOT, "i", flags);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_OPEN_ROOT>(0x%x):%p:%d request: %s", flags, gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_open_root_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_OPEN_ROOT>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_open_parernt_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_uint32_t flags)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_OPEN_PARENT, "i", flags));
}

gfarm_error_t
gfm_client_open_parent_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_close_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_CLOSE, ""));
}

gfarm_error_t
gfm_client_close_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_verify_type_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t type)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_VERIFY_TYPE,
	    "i", type));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_VERIFY_TYPE,
	    "i", type);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_VERIFY_TYPE>(%d):%p:%d request: %s", type, gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_verify_type_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_VERIFY_TYPE>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_verify_type_not_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t type)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_VERIFY_TYPE_NOT, "i", type));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_VERIFY_TYPE_NOT, "i", type);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_VERIFY_TYPE_NOT>(%d):%p:%d request: %s", type, gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_verify_type_not_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_VERIFY_TYPE_NOT>:%p:%d result: %s", gfm_server->conn, xid, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_revoke_gfsd_access(struct gfm_connection *gfm_server,
	gfarm_int32_t fd)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_REVOKE_GFSD_ACCESS, "i/", fd));
}

gfarm_error_t
gfm_client_bequeath_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_BEQUEATH_FD, ""));
}

gfarm_error_t
gfm_client_bequeath_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_inherit_fd_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t fd)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_INHERIT_FD, "i", fd));
}

gfarm_error_t
gfm_client_inherit_fd_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_fstat_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_FSTAT, ""));
}

gfarm_error_t
gfm_client_fstat_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, struct gfs_stat *st)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "llilsslllilili",
	    &st->st_ino, &st->st_gen, &st->st_mode, &st->st_nlink,
	    &st->st_user, &st->st_group, &st->st_size,
	    &st->st_ncopy,
	    &st->st_atimespec.tv_sec, &st->st_atimespec.tv_nsec,
	    &st->st_mtimespec.tv_sec, &st->st_mtimespec.tv_nsec,
	    &st->st_ctimespec.tv_sec, &st->st_ctimespec.tv_nsec));
}

gfarm_error_t
gfm_client_fgetattrplus_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, char **patterns, int npatterns, int flags)
{
	gfarm_error_t e;
	int i, size_pos;

	if ((e = gfm_client_rpc_request_begin(gfm_server, ctx, &size_pos,
	    GFM_PROTO_FGETATTRPLUS, "ii", flags, npatterns)) !=
	    GFARM_ERR_NO_ERROR)
		return (e);

	for (i = 0; i < npatterns; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", patterns[i]);
		if (e != GFARM_ERR_NO_ERROR)
			return (e); /* XXX xid memory leak */
	}
	if ((e = gfm_client_rpc_request_end(gfm_server, ctx, size_pos,
	    GFM_PROTO_FGETATTRPLUS)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_fgetattrplus_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, struct gfs_stat *st, int *nattrsp,
	char ***attrnamesp, void ***attrvaluesp, size_t **attrsizesp)
{
	gfarm_error_t e;
	size_t size;
	int j, nattrs;
	char **attrs;
	void **values;
	size_t *sizes;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size,
	    "llilsslllililii",
	    &st->st_ino, &st->st_gen, &st->st_mode, &st->st_nlink,
	    &st->st_user, &st->st_group, &st->st_size,
	    &st->st_ncopy,
	    &st->st_atimespec.tv_sec, &st->st_atimespec.tv_nsec,
	    &st->st_mtimespec.tv_sec, &st->st_mtimespec.tv_nsec,
	    &st->st_ctimespec.tv_sec, &st->st_ctimespec.tv_nsec,
	    &nattrs);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002454, "gfm_client_fgetattrplus: "
		    "gfm_client_rpc_result_begin(): %s",
		    gfarm_error_string(e));
		return (e);
	}
	GFARM_CALLOC_ARRAY(attrs, nattrs);
	GFARM_CALLOC_ARRAY(values, nattrs);
	GFARM_CALLOC_ARRAY(sizes, nattrs);
	if (attrs == NULL || values == NULL || sizes == NULL) {
		/* XXX debug output */
		nattrs = 0;
		free(attrs);
		free(values);
		free(sizes);
		attrs = NULL;
		values = NULL;
		sizes = NULL;
	}
	for (j = 0; j < nattrs; j++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "sB",
		    &attrs[j], &sizes[j], &values[j]);
		if (e != GFARM_ERR_NO_ERROR) {
			/* XXX memory leak */
			gflog_debug(GFARM_MSG_1002455,
				"gfm_client_fgetattrplus: %s",
				gfarm_error_string(e));
			nattrs = j;
			break;
		}
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}
	*nattrsp = nattrs;
	*attrnamesp = attrs;
	*attrvaluesp = values;
	*attrsizesp = sizes;

	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_futimes_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec,
	gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_FUTIMES,
	    "lili", atime_sec, atime_nsec, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfm_client_futimes_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_fchmod_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_mode_t mode)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_FCHMOD,
	    "i", mode));
}

gfarm_error_t
gfm_client_fchmod_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_fchown_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *user, const char *group)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_FCHOWN, "ss",
	    user == NULL ? "" : user,
	    group == NULL ? "" : group));
}

gfarm_error_t
gfm_client_fchown_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_cksum_get_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_CKSUM_GET, ""));
}

gfarm_error_t
gfm_client_cksum_get_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	char **cksum_typep, size_t bufsize, size_t *cksum_lenp, char *cksum,
	gfarm_int32_t *flagsp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "sbi",
	    cksum_typep, bufsize, cksum_lenp, cksum, flagsp));
}

gfarm_error_t
gfm_client_cksum_set_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	char *cksum_type, size_t cksum_len, const char *cksum,
	gfarm_int32_t flags, gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_CKSUM_SET, "sbili", cksum_type, cksum_len, cksum,
	    flags, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfm_client_cksum_set_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

void
gfarm_host_sched_info_free(int nhosts, struct gfarm_host_sched_info *infos)
{
	int i;

	for (i = 0; i < nhosts; i++)
		free(infos[i].host);
	free(infos);
}

/* this interface is exported for a use from a private extension */
gfarm_error_t
gfm_client_get_schedule_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *nhostsp, struct gfarm_host_sched_info **infosp)
{
	gfarm_error_t e;
	gfarm_int32_t i, nhosts, loadavg;
	struct gfarm_host_sched_info *infos;
	size_t size;

	if ((e = gfm_client_rpc_result_begin(gfm_server, ctx, &size,
	    "i", &nhosts)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001146,
		    "gfm_client_rpc_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	GFARM_MALLOC_ARRAY(infos, nhosts);
	if (infos == NULL) { /* XXX this breaks gfm protocol */
		gflog_debug(GFARM_MSG_1001147,
			"allocation of array 'gfarm_host_sched_info' failed: "
			"%s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}

	for (i = 0; i < nhosts; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "siiillllii",
		    &infos[i].host, &infos[i].port, &infos[i].ncpu,
		    &loadavg, &infos[i].cache_time,
		    &infos[i].disk_used, &infos[i].disk_avail,
		    &infos[i].rtt_cache_time,
		    &infos[i].rtt_usec, &infos[i].flags);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001148,
				"receiving host schedule response failed: %s",
				gfarm_error_string(e));
			return (e); /* XXX memory leak */
		}
		/* loadavg_1min * GFM_PROTO_LOADAVG_FSCALE */
		infos[i].loadavg = loadavg;
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}

	*nhostsp = nhosts;
	*infosp = infos;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_schedule_file_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, const char *domain)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_SCHEDULE_FILE, "s", domain));
}

gfarm_error_t
gfm_client_schedule_file_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *nhostsp, struct gfarm_host_sched_info **infosp)
{
	return (gfm_client_get_schedule_result(gfm_server, ctx,
	    nhostsp, infosp));
}

gfarm_error_t
gfm_client_schedule_file_with_program_request(
	struct gfm_connection *gfm_server, struct gfp_xdr_context *ctx,
	const char *domain)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_SCHEDULE_FILE_WITH_PROGRAM, "s", domain));
}

gfarm_error_t
gfm_client_schedule_file_with_program_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *nhostsp, struct gfarm_host_sched_info **infosp)
{
	return (gfm_client_get_schedule_result(gfm_server, ctx,
	    nhostsp, infosp));
}

gfarm_error_t
gfm_client_schedule_host_domain(struct gfm_connection *gfm_server,
	const char *domain,
	int *nhostsp, struct gfarm_host_sched_info **infosp)
{
	gfarm_error_t e;
	struct gfp_xdr_context *ctx;

	if ((e = gfp_xdr_context_alloc(gfm_server->conn, &ctx)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED, "gfp_xdr_context_alloc: %s",
		    gfarm_error_string(e));
		return (e);
	}

	e = gfm_client_rpc_request(gfm_server, ctx,
		GFM_PROTO_SCHEDULE_HOST_DOMAIN, "s", domain);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001150,
			"gfm_client_rpc() request failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	e = gfm_client_get_schedule_result(gfm_server, ctx,
	    nhostsp, infosp);

	gfp_xdr_context_free(gfm_server->conn, ctx);

	return (e);
}

gfarm_error_t
gfm_client_statfs(struct gfm_connection *gfm_server,
	gfarm_off_t *used, gfarm_off_t *avail, gfarm_off_t *files)
{
	return (gfm_client_rpc(gfm_server,
		    GFM_PROTO_STATFS, "/lll", used, avail, files));
}

gfarm_error_t
gfm_client_remove_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, const char *name)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_REMOVE, "s",
	    name));
}

gfarm_error_t
gfm_client_remove_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_rename_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *src_name, const char *target_name)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_RENAME, "ss",
	    src_name, target_name));
}

gfarm_error_t
gfm_client_rename_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_flink_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, const char *target_name)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_FLINK, "s",
	    target_name));
}

gfarm_error_t
gfm_client_flink_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_mkdir_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *target_name, gfarm_mode_t mode)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_MKDIR, "si",
	    target_name, mode));
}

gfarm_error_t
gfm_client_mkdir_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_symlink_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *target_path, const char *link_name)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_SYMLINK, "ss", target_path, link_name));
}

gfarm_error_t
gfm_client_symlink_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_readlink_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_READLINK, ""));
#else
	gfarm_error_t e;
	e = gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_READLINK, "");
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_READLINK>:%p:%d request: %s", gfm_server->conn, e == GFARM_ERR_NO_ERROR ? gfp_xdr_client_last_xid(ctx) : 0, gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_readlink_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, char **linkp)
{
#ifndef GFM_PROTO_OPEN_DEBUG
	return (gfm_client_rpc_result(gfm_server, ctx, "s", linkp));
#else
	gfarm_error_t e;
	gfarm_uint32_t xid = gfp_xdr_client_first_xid(ctx);
	e = gfm_client_rpc_result(gfm_server, ctx, "s", linkp);
	gflog_info(GFARM_MSG_UNFIXED, "<GFM_PROTO_READLINK>:%p:%d result=>%s: %s", gfm_server->conn, xid, e == GFARM_ERR_NO_ERROR ? *linkp : "N/A", gfarm_error_string(e));
	return (e);
#endif
}

gfarm_error_t
gfm_client_getdirpath_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_GETDIRPATH, ""));
}

gfarm_error_t
gfm_client_getdirpath_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, char **pathp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "s", pathp));
}

gfarm_error_t
gfm_client_getdirents_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t n_entries)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_GETDIRENTS, "i", n_entries));
}

gfarm_error_t
gfm_client_getdirents_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *n_entriesp, struct gfs_dirent *dirents)
{
	gfarm_error_t e;
	int i;
	gfarm_int32_t n, type;
	size_t size, sz;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001151,
		    "gfm_client_rpc_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < n; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "bil",
		    sizeof(dirents[i].d_name) - 1, &sz, dirents[i].d_name,
		    &type,
		    &dirents[i].d_fileno);
		if (e != GFARM_ERR_NO_ERROR) {
			/* XXX memory leak */
			gflog_debug(GFARM_MSG_1001152,
				"receiving getdireents response failed: %s",
				gfarm_error_string(e));
			return (e);
		}
		if (sz >= sizeof(dirents[i].d_name) - 1)
			sz = sizeof(dirents[i].d_name) - 1;
		dirents[i].d_name[sz] = '\0';
		dirents[i].d_namlen = sz;
		dirents[i].d_type = type;
		/* XXX */
		dirents[i].d_reclen =
		    sizeof(dirents[i]) - sizeof(dirents[i].d_name) + sz;
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}
	*n_entriesp = n;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_getdirentsplus_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t n_entries)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_GETDIRENTSPLUS, "i", n_entries));
}

gfarm_error_t
gfm_client_getdirentsplus_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *n_entriesp, struct gfs_dirent *dirents, struct gfs_stat *stv)
{
	gfarm_error_t e;
	int i;
	gfarm_int32_t n;
	size_t size, sz;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001153,
			"gfm_client_rpc_result() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < n; i++) {
		struct gfs_stat *st = &stv[i];

		e = gfm_client_xdr_recv(gfm_server, &size, "bllilsslllilili",
		    sizeof(dirents[i].d_name) - 1, &sz, dirents[i].d_name,
		    &st->st_ino, &st->st_gen, &st->st_mode, &st->st_nlink,
		    &st->st_user, &st->st_group, &st->st_size,
		    &st->st_ncopy,
		    &st->st_atimespec.tv_sec, &st->st_atimespec.tv_nsec,
		    &st->st_mtimespec.tv_sec, &st->st_mtimespec.tv_nsec,
		    &st->st_ctimespec.tv_sec, &st->st_ctimespec.tv_nsec);
		/* XXX st_user or st_group may be NULL */
		if (e != GFARM_ERR_NO_ERROR) {
			/* XXX memory leak */
			gflog_debug(GFARM_MSG_1001154,
				"receiving getdirentsplus response failed: %s",
				gfarm_error_string(e));
			return (e);
		}
		if (sz >= sizeof(dirents[i].d_name) - 1)
			sz = sizeof(dirents[i].d_name) - 1;
		dirents[i].d_name[sz] = '\0';
		dirents[i].d_namlen = sz;
		dirents[i].d_type = gfs_mode_to_type(st->st_mode);
		/* XXX */
		dirents[i].d_reclen =
		    sizeof(dirents[i]) - sizeof(dirents[i].d_name) + sz;
		dirents[i].d_fileno = st->st_ino;
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}
	*n_entriesp = n;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_getdirentsplusxattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t n_entries, char **patterns, int npatterns)
{
	gfarm_error_t e;
	int i, size_pos;

	if ((e = gfm_client_rpc_request_begin(gfm_server, ctx, &size_pos,
	    GFM_PROTO_GETDIRENTSPLUSXATTR, "ii", n_entries, npatterns)) !=
	    GFARM_ERR_NO_ERROR)
		return (e);

	for (i = 0; i < npatterns; i++) {
		e = gfm_client_xdr_send(gfm_server, "s", patterns[i]);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	}
	if ((e = gfm_client_rpc_request_end(gfm_server, ctx, size_pos,
	    GFM_PROTO_GETDIRENTSPLUSXATTR)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_rpc_request_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX xid memory leak */
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_getdirentsplusxattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int *n_entriesp, struct gfs_dirent *dirents, struct gfs_stat *stv,
	int *nattrsv, char ***attrsv, void ***valuesv, size_t **sizesv)
{
	gfarm_error_t e;
	int i, j, nattrs;
	gfarm_int32_t n;
	char **attrs;
	void **values;
	size_t size, sz, *sizes;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001153,
			"gfm_client_rpc_result() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	for (i = 0; i < n; i++) {
		struct gfs_stat *st = &stv[i];

		e = gfm_client_xdr_recv(gfm_server, &size, "bllilsslllililii",
		    sizeof(dirents[i].d_name) - 1, &sz, dirents[i].d_name,
		    &st->st_ino, &st->st_gen, &st->st_mode, &st->st_nlink,
		    &st->st_user, &st->st_group, &st->st_size,
		    &st->st_ncopy,
		    &st->st_atimespec.tv_sec, &st->st_atimespec.tv_nsec,
		    &st->st_mtimespec.tv_sec, &st->st_mtimespec.tv_nsec,
		    &st->st_ctimespec.tv_sec, &st->st_ctimespec.tv_nsec,
		    &nattrsv[i]);
		/* XXX st_user or st_group may be NULL */
		if (e != GFARM_ERR_NO_ERROR) {
			/* XXX memory leak */
			gflog_debug(GFARM_MSG_1002456,
				"getdirentsplusxattr response: %s",
				gfarm_error_string(e));
			return (e);
		}
		if (sz >= sizeof(dirents[i].d_name) - 1)
			sz = sizeof(dirents[i].d_name) - 1;
		dirents[i].d_name[sz] = '\0';
		dirents[i].d_namlen = sz;
		dirents[i].d_type = gfs_mode_to_type(st->st_mode);
		/* XXX */
		dirents[i].d_reclen =
		    sizeof(dirents[i]) - sizeof(dirents[i].d_name) + sz;
		dirents[i].d_fileno = st->st_ino;
		nattrs = nattrsv[i];
		GFARM_CALLOC_ARRAY(attrs, nattrs);
		GFARM_CALLOC_ARRAY(values, nattrs);
		GFARM_CALLOC_ARRAY(sizes, nattrs);
		if (attrs == NULL || values == NULL || sizes == NULL) {
			/* XXX debug output */
			nattrs = 0;
			free(attrs);
			free(values);
			free(sizes);
			attrs = NULL;
			values = NULL;
			sizes = NULL;
		}
		for (j = 0; j < nattrs; j++) {
			e = gfm_client_xdr_recv(gfm_server, &size, "sB",
			    &attrs[j], &sizes[j], &values[j]);
			if (e != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_NO_ERROR)
					e = GFARM_ERR_PROTOCOL;
				/* XXX memory leak */
				gflog_debug(GFARM_MSG_1002457,
					"getdirentsplusxattr xattr: %s",
					gfarm_error_string(e));
				nattrs = j;
				break;
			}
		}
		nattrsv[i] = nattrs;
		attrsv[i] = attrs;
		sizesv[i] = sizes;
		valuesv[i] = values;
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}
	*n_entriesp = n;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_seek_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_off_t offset, gfarm_int32_t whence)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_SEEK, "li",
	    offset, whence));
}

gfarm_error_t
gfm_client_seek_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_off_t *offsetp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "l", offsetp));
}

/*
 * quota
 */
static gfarm_error_t
gfm_client_quota_get_common(struct gfm_connection *gfm_server, int proto,
			    const char *name, struct gfarm_quota_get_info *qi)
{
	return (gfm_client_rpc(
			gfm_server, proto,
			"s/slllllllllllllllll",
			name,
			&qi->name,
			&qi->grace_period,
			&qi->space,
			&qi->space_grace,
			&qi->space_soft,
			&qi->space_hard,
			&qi->num,
			&qi->num_grace,
			&qi->num_soft,
			&qi->num_hard,
			&qi->phy_space,
			&qi->phy_space_grace,
			&qi->phy_space_soft,
			&qi->phy_space_hard,
			&qi->phy_num,
			&qi->phy_num_grace,
			&qi->phy_num_soft,
			&qi->phy_num_hard));
}

static gfarm_error_t
gfm_client_quota_set_common(struct gfm_connection *gfm_server, int proto,
			    struct gfarm_quota_set_info *qi) {
	return (gfm_client_rpc(
			gfm_server, proto,
			"slllllllll/",
			qi->name,
			qi->grace_period,
			qi->space_soft,
			qi->space_hard,
			qi->num_soft,
			qi->num_hard,
			qi->phy_space_soft,
			qi->phy_space_hard,
			qi->phy_num_soft,
			qi->phy_num_hard));
}

gfarm_error_t
gfm_client_quota_user_get(struct gfm_connection *gfm_server,
			  const char *name, struct gfarm_quota_get_info *qi)
{
	return (gfm_client_quota_get_common(
			gfm_server, GFM_PROTO_QUOTA_USER_GET, name, qi));
}

gfarm_error_t
gfm_client_quota_user_set(struct gfm_connection *gfm_server,
			  struct gfarm_quota_set_info *qi) {
	return (gfm_client_quota_set_common(
			gfm_server, GFM_PROTO_QUOTA_USER_SET, qi));
}

gfarm_error_t
gfm_client_quota_group_get(struct gfm_connection *gfm_server,
			   const char *name, struct gfarm_quota_get_info *qi)
{
	return (gfm_client_quota_get_common(
			gfm_server, GFM_PROTO_QUOTA_GROUP_GET, name, qi));
}

gfarm_error_t
gfm_client_quota_group_set(struct gfm_connection *gfm_server,
			   struct gfarm_quota_set_info *qi)
{
	return (gfm_client_quota_set_common(
			gfm_server, GFM_PROTO_QUOTA_GROUP_SET, qi));
}

gfarm_error_t
gfm_client_quota_check(struct gfm_connection *gfm_server)
{
	return (gfm_client_rpc(gfm_server, GFM_PROTO_QUOTA_CHECK, "/"));
}

/*
 * extended attributes
 */
gfarm_error_t
gfm_client_setxattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int xmlMode, const char *name, const void *value, size_t size,
	int flags)
{
	int command = xmlMode ? GFM_PROTO_XMLATTR_SET : GFM_PROTO_XATTR_SET;
	return (gfm_client_rpc_request(gfm_server, ctx, command, "sbi",
	    name, size, value, flags));
}

gfarm_error_t
gfm_client_setxattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_getxattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int xmlMode, const char *name)
{
	int command = xmlMode ? GFM_PROTO_XMLATTR_GET : GFM_PROTO_XATTR_GET;
	return (gfm_client_rpc_request(gfm_server, ctx, command, "s", name));
}

gfarm_error_t
gfm_client_getxattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	int xmlMode, void **valuep, size_t *size)
{
	gfarm_error_t e;

	e = gfm_client_rpc_result(gfm_server, ctx, "B", size, valuep);
	if ((e == GFARM_ERR_NO_ERROR) && xmlMode) {
		/* value is text with '\0', drop it */
		(*size)--;
	}
	return (e);
}

gfarm_error_t
gfm_client_listxattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int xmlMode)
{
	int command = xmlMode ? GFM_PROTO_XMLATTR_LIST : GFM_PROTO_XATTR_LIST;
	return (gfm_client_rpc_request(gfm_server, ctx, command, ""));
}

gfarm_error_t
gfm_client_listxattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	char **listp, size_t *size)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "B", size, listp));
}

gfarm_error_t
gfm_client_removexattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, int xmlMode, const char *name)
{
	int command = xmlMode ? GFM_PROTO_XMLATTR_REMOVE :
		GFM_PROTO_XATTR_REMOVE;
	return (gfm_client_rpc_request(gfm_server, ctx, command, "s", name));
}

gfarm_error_t
gfm_client_removexattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_findxmlattr_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, struct gfs_xmlattr_ctx *ctxp)
{
	char *path, *attrname;

	if (ctxp->cookie_path != NULL) {
		path = ctxp->cookie_path;
		attrname = ctxp->cookie_attrname;
	} else {
		path = attrname = "";
	}

	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_XMLATTR_FIND,
	    "siiss", ctxp->expr, ctxp->depth, ctxp->nalloc, path, attrname));
}

gfarm_error_t
gfm_client_findxmlattr_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, struct gfs_xmlattr_ctx *ctxp)
{
	gfarm_error_t e, e2;
	int i;
	size_t size;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "ii",
			&ctxp->eof, &ctxp->nvalid);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001155,
		    "gfm_client_rpc_result_begin() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (ctxp->nvalid > ctxp->nalloc) {
		gflog_debug(GFARM_MSG_1001156,
			"ctx.nvalid > ctx.nalloc: %s",
			gfarm_error_string(GFARM_ERR_UNKNOWN));
		e = GFARM_ERR_UNKNOWN;
		goto error;
	}
	for (i = 0; i < ctxp->nvalid; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "ss",
			&ctxp->entries[i].path, &ctxp->entries[i].attrname);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001157,
				"receiving ctx.entries response failed: %s",
				gfarm_error_string(e));
			goto error;
		}
	}

	if ((ctxp->eof == 0) && (ctxp->nvalid > 0)) {
		free(ctxp->cookie_path);
		free(ctxp->cookie_attrname);
		ctxp->cookie_path = strdup(ctxp->entries[ctxp->nvalid-1].path);
		ctxp->cookie_attrname =
			strdup(ctxp->entries[ctxp->nvalid-1].attrname);
		if ((ctxp->cookie_path == NULL) ||
		    (ctxp->cookie_attrname == NULL)) {
			gflog_debug(GFARM_MSG_1001158,
				"allocation of 'ctx.cookie_path' or "
				"'cookie_attrname' failed: %s",
				gfarm_error_string(GFARM_ERR_NO_MEMORY));
			e = GFARM_ERR_NO_MEMORY;
			goto error;
		}
	}

error:
	if ((e2 = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e2));
		return (e2); /* XXX memory leak */
	}
	return (e);

	
}

/*
 * gfs from gfsd
 */

gfarm_error_t
gfm_client_reopen_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_REOPEN, ""));
}

gfarm_error_t
gfm_client_reopen_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_ino_t *ino_p, gfarm_uint64_t *gen_p, gfarm_int32_t *modep,
	gfarm_int32_t *flagsp, gfarm_int32_t *to_create_p)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "lliii", ino_p, gen_p,
	    modep, flagsp, to_create_p));
}

gfarm_error_t
gfm_client_close_read_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_CLOSE_READ,
	    "li", atime_sec, atime_nsec));
}

gfarm_error_t
gfm_client_close_read_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

#ifdef COMPAT_GFARM_2_3
gfarm_error_t
gfm_client_close_write_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t size,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec,
	gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_CLOSE_WRITE,
	    "llili", size, atime_sec, atime_nsec, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfm_client_close_write_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}
#endif

gfarm_error_t
gfm_client_close_write_v2_4_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t size,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec,
	gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_CLOSE_WRITE_V2_4,
	    "llili", size, atime_sec, atime_nsec, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfm_client_close_write_v2_4_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t *flagsp,
	gfarm_int64_t *old_igenp, gfarm_int64_t *new_igenp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "ill",
	    flagsp, old_igenp, new_igenp));
}

gfarm_error_t
gfm_client_fhclose_read(struct gfm_connection *gfm_server,
	gfarm_ino_t inode, gfarm_uint64_t gen,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec)
{
	return (gfm_client_rpc(gfm_server, GFM_PROTO_FHCLOSE_READ,
	    "llli/", inode, gen, atime_sec, atime_nsec));
}

gfarm_error_t
gfm_client_fhclose_write(struct gfm_connection *gfm_server,
	gfarm_ino_t inode, gfarm_uint64_t gen, gfarm_off_t size,
	gfarm_int64_t atime_sec, gfarm_int32_t atime_nsec,
	gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec,
	gfarm_int32_t *flagsp,
	gfarm_int64_t *old_igenp, gfarm_int64_t *new_igenp,
	gfarm_uint64_t *cookiep)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_FHCLOSE_WRITE, "llllili/illl",
	    inode, gen, size, atime_sec, atime_nsec, mtime_sec, mtime_nsec,
	    flagsp, old_igenp, new_igenp, cookiep));
}

gfarm_error_t
gfm_client_generation_updated_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t errcode)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_GENERATION_UPDATED, "i", errcode));
}

gfarm_error_t
gfm_client_generation_updated_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_generation_updated_by_cookie(
	struct gfm_connection *gfm_server,
	gfarm_uint64_t cookie, gfarm_int32_t errcode)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_GENERATION_UPDATED_BY_COOKIE, "li/", cookie, errcode));
}

gfarm_error_t
gfm_client_lock_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfm_client_rpc_request(gfm_server, ctx, GFM_PROTO_LOCK, "llii",
	    start, len, type, whence));
}

gfarm_error_t
gfm_client_lock_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_trylock_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_TRYLOCK, "llii",
	    start, len, type, whence));
}

gfarm_error_t
gfm_client_trylock_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_unlock_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_UNLOCK, "llii", start, len, type, whence));
}

gfarm_error_t
gfm_client_unlock_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_lock_info_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t start, gfarm_off_t len,
	gfarm_int32_t type, gfarm_int32_t whence)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_LOCK_INFO, "llii",
	    start, len, type, whence));
}

gfarm_error_t
gfm_client_lock_info_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_off_t *startp, gfarm_off_t *lenp, gfarm_int32_t *typep,
	char **hostp, gfarm_pid_t *pidp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "llisl",
	    startp, lenp, typep, hostp, pidp));
}

#ifdef COMPAT_GFARM_2_3
gfarm_error_t
gfm_client_switch_back_channel(struct gfm_connection *gfm_server)
{
	return (gfm_client_rpc(gfm_server, 0,
	    GFM_PROTO_SWITCH_BACK_CHANNEL, "/"));
}
#endif

static gfarm_error_t
setsockopt_to_async_channel(struct gfm_connection *gfm_server,
	const char *diag)
{
	gfarm_error_t e;

	e = gfarm_sockopt_set_option(
	    gfp_xdr_fd(gfm_server->conn), "tcp_nodelay");
	if (e == GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1003414, "tcp_nodelay is specified "
		    "for async channel %s", diag);
	else
		gflog_error(GFARM_MSG_1002574,
		    "setting TCP_NODELAY for %s failed, slow", diag);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_switch_async_back_channel(struct gfm_connection *gfm_server,
	gfarm_int32_t version, gfarm_int64_t gfsd_cookie,
	gfarm_int32_t *gfmd_knows_me_p)
{
	gfarm_error_t e = gfm_client_rpc(gfm_server,
	    GFM_PROTO_SWITCH_ASYNC_BACK_CHANNEL, "il/i",
	    version, gfsd_cookie, gfmd_knows_me_p);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	return (setsockopt_to_async_channel(gfm_server, "back channel"));
}

gfarm_error_t
gfm_client_switch_gfmd_channel(struct gfm_connection *gfm_server,
	gfarm_int32_t version, gfarm_int64_t gfmd_cookie,
	gfarm_int32_t *gfmd_knows_me_p)
{
	gfarm_error_t e = gfm_client_rpc(gfm_server,
	    GFM_PROTO_SWITCH_GFMD_CHANNEL, "il/i",
	    version, gfmd_cookie, gfmd_knows_me_p);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	return (setsockopt_to_async_channel(gfm_server, "gfmd channel"));
}

/*
 * gfs_pio from client
 */

gfarm_error_t
gfm_client_glob(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001159,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

gfarm_error_t
gfm_client_schedule(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001160,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

gfarm_error_t
gfm_client_pio_open(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001161,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

gfarm_error_t
gfm_client_pio_set_paths(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001162,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

gfarm_error_t
gfm_client_pio_close(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001163,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

gfarm_error_t
gfm_client_pio_visit(struct gfm_connection *gfm_server)
{
	/* XXX - NOT IMPLEMENTED */
	gflog_debug(GFARM_MSG_1001164,
		"Not implemented: %s",
		gfarm_error_string(GFARM_ERR_FUNCTION_NOT_IMPLEMENTED));
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

/*
 * misc operations from gfsd
 */

gfarm_error_t
gfm_client_hostname_set(struct gfm_connection *gfm_server, const char *hostname)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_HOSTNAME_SET, "s/", hostname));
}

/*
 * replica management from client
 */

gfarm_error_t
gfm_client_replica_list_by_name_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_LIST_BY_NAME, ""));
}

gfarm_error_t
gfm_client_replica_list_by_name_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t *n_replicasp, char ***replica_hosts)
{
	gfarm_error_t e;
	int i;
	size_t size;
	gfarm_int32_t n;
	char **hosts;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001165,
		    "gfm_client_rpc_result() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	GFARM_MALLOC_ARRAY(hosts, n);
	if (hosts == NULL) {
		gflog_debug(GFARM_MSG_1001166,
		    "allocation of array 'hosts' failed: %s",
		    gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY); /* XXX not graceful */
	}

	for (i = 0; i < n; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size,  "s", &hosts[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001167,
				"receiving host response failed: %s",
				gfarm_error_string(e));
			break;
		}
	}
	if (i < n) {
		for (; i >= 0; --i)
			free(hosts[i]);
		free(hosts);
		return (e);
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	*n_replicasp = n;
	*replica_hosts = hosts;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_replica_list_by_host_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *host, gfarm_int32_t port)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_LIST_BY_HOST, "si", host, port));
}

gfarm_error_t
gfm_client_replica_list_by_host_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t *n_replicasp, gfarm_ino_t **inodesp)
{
	gfarm_error_t e;
	size_t size;
	int i;
	gfarm_int32_t n;
	gfarm_ino_t *inodes;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001168,
		    "gfm_client_rpc_result_begin() failed: %s",
		gfarm_error_string(e));
		return (e);
	}
	GFARM_MALLOC_ARRAY(inodes, n);
	if (inodes == NULL) {
		gflog_debug(GFARM_MSG_1001169,
			"allocation of array 'inodes' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY); /* XXX not graceful */
	}
	for (i = 0; i < n; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "l", &inodes[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			free(inodes);
			gflog_debug(GFARM_MSG_1001170,
				"receiving inode response failed: %s",
				gfarm_error_string(e));
			return (e);
		}
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	*n_replicasp = n;
	*inodesp = inodes;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_replica_remove_by_host_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *host, gfarm_int32_t port)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_REMOVE_BY_HOST, "si", host, port));
}

gfarm_error_t
gfm_client_replica_remove_by_host_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_replica_remove_by_file_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *host)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_REMOVE_BY_FILE, "s", host));
}

gfarm_error_t
gfm_client_replica_remove_by_file_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_replica_info_get_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, gfarm_int32_t flags)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_INFO_GET, "i", flags));
}

gfarm_error_t
gfm_client_replica_info_get_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t *np,
	char ***hostsp, gfarm_uint64_t **gensp, gfarm_int32_t **flagsp)
{
	gfarm_error_t e;
	size_t size;
	int i;
	gfarm_int32_t n;
	char **hosts;
	gfarm_uint64_t *gens;
	gfarm_int32_t *flags;

	e = gfm_client_rpc_result_begin(gfm_server, ctx, &size, "i", &n);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	GFARM_MALLOC_ARRAY(hosts, n);
	if (hosts == NULL)
		return (GFARM_ERR_NO_MEMORY); /* XXX not graceful */
	GFARM_MALLOC_ARRAY(gens, n);
	if (gens == NULL) {
		free(hosts);
		return (GFARM_ERR_NO_MEMORY); /* XXX not graceful */
	}
	GFARM_MALLOC_ARRAY(flags, n);
	if (flags == NULL) {
		free(gens);
		free(hosts);
		return (GFARM_ERR_NO_MEMORY); /* XXX not graceful */
	}

	for (i = 0; i < n; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "sli",
		    &hosts[i], &gens[i], &flags[i]);
		if (e != GFARM_ERR_NO_ERROR)
			break;
	}
	if (i < n) {
		for (; i >= 0; --i)
			free(hosts[i]);
		free(flags);
		free(gens);
		free(hosts);
		return (e);
	}
	if ((e = gfm_client_rpc_result_end(gfm_server, ctx, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	*np = n;
	*hostsp = hosts;
	*gensp = gens;
	*flagsp = flags;
	return (GFARM_ERR_NO_ERROR);

}

gfarm_error_t
gfm_client_replicate_file_from_to_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *srchost, const char *dsthost, gfarm_int32_t flags)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICATE_FILE_FROM_TO, "ssi", srchost, dsthost, flags));
}

gfarm_error_t
gfm_client_replicate_file_from_to_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_replicate_file_to_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	const char *dsthost, gfarm_int32_t flags)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICATE_FILE_TO, "si", dsthost, flags));
}

gfarm_error_t
gfm_client_replicate_file_to_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

/*
 * replica management from gfsd
 */

gfarm_error_t
gfm_client_replica_adding_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx, char *src_host)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_ADDING, "s", src_host));
}

gfarm_error_t
gfm_client_replica_adding_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_ino_t *ino_p, gfarm_uint64_t *gen_p,
	gfarm_int64_t *mtime_secp, gfarm_int32_t *mtime_nsecp)
{
	return (gfm_client_rpc_result(gfm_server, ctx, "llli",
	    ino_p, gen_p, mtime_secp, mtime_nsecp));
}

#ifdef COMPAT_GFARM_2_3
gfarm_error_t
gfm_client_replica_added_request(struct gfm_connection *gfm_server,
	gfarm_int32_t flags, gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec)
{
	return (gfm_client_rpc_request(gfm_server,
	    GFM_PROTO_REPLICA_ADDED, "ili", flags, mtime_sec, mtime_nsec));
}
#endif

gfarm_error_t
gfm_client_replica_added2_request(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx,
	gfarm_int32_t flags, gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec,
	gfarm_off_t size)
{
	return (gfm_client_rpc_request(gfm_server, ctx,
	    GFM_PROTO_REPLICA_ADDED2, "ilil",
	    flags, mtime_sec, mtime_nsec, size));
}

gfarm_error_t
gfm_client_replica_added2_result(struct gfm_connection *gfm_server,
	struct gfp_xdr_context *ctx)
{
	return (gfm_client_rpc_result(gfm_server, ctx, ""));
}

gfarm_error_t
gfm_client_replica_lost(struct gfm_connection *gfm_server,
	gfarm_ino_t inum, gfarm_uint64_t gen)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_REPLICA_LOST, "ll/", inum, gen));
}

gfarm_error_t
gfm_client_replica_add(struct gfm_connection *gfm_server,
	gfarm_ino_t inum, gfarm_uint64_t gen, gfarm_off_t size)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_REPLICA_ADD, "lll/", inum, gen, size));
}

gfarm_error_t
gfm_client_replica_get_my_entries(struct gfm_connection *gfm_server,
	gfarm_ino_t inum, int n,
	int *np,
	gfarm_ino_t **inumsp, gfarm_uint64_t **gensp, gfarm_off_t **sizesp)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	int i;
	gfarm_ino_t *inums;
	gfarm_uint64_t *gens;
	gfarm_off_t *sizes;

	e = gfm_client_rpc_request_and_result_begin(gfm_server, &xidr, &size,
	    GFM_PROTO_REPLICA_GET_MY_ENTRIES2, "li/i", inum, n, &n);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1003451,
		    "gfm_client_rpc_request_and_result_begin(): %s",
		    gfarm_error_string(e));
		return (e);
	} else if (n <= 0)
		return (GFARM_ERR_NO_SUCH_OBJECT);

	GFARM_MALLOC_ARRAY(inums, n);
	GFARM_MALLOC_ARRAY(gens, n);
	GFARM_MALLOC_ARRAY(sizes, n);
	if (inums == NULL || gens == NULL || sizes == NULL) {
		free(inums);
		free(gens);
		free(sizes);
		return (GFARM_ERR_NO_MEMORY); /* XXX FIXME: not graceful */
	}
	for (i = 0; i < n; i++) {
		e = gfm_client_xdr_recv(gfm_server, &size, "lll",
		    &inums[i], &gens[i], &sizes[i]);
		if (IS_CONNECTION_ERROR(e))
			gfm_client_purge_from_cache(gfm_server);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1003452,
			    "gfm_client_xdr_recv(): %s", gfarm_error_string(e));
			free(inums);
			free(gens);
			free(sizes);
			return (e); /* XXX xid memory leak */
		}
	}
	if ((e = gfm_client_rpc_raw_result_end(gfm_server, xidr, size)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "get_client_rpc_raw_result_end() failed: %s",
		    gfarm_error_string(e));
		return (e); /* XXX memory leak */
	}
	*np = n;
	*inumsp = inums;
	*gensp = gens;
	*sizesp = sizes;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_client_replica_create_file_in_lost_found(
	struct gfm_connection *gfm_server,
	gfarm_ino_t inum_old, gfarm_uint64_t gen_old, gfarm_off_t size,
	const struct gfarm_timespec *mtime,
	gfarm_ino_t *inum_newp, gfarm_uint64_t *gen_newp)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_REPLICA_CREATE_FILE_IN_LOST_FOUND, "lllli/ll",
	    inum_old, gen_old, size, mtime->tv_sec, mtime->tv_nsec,
	    inum_newp, gen_newp));
}

/*
 * process management
 */

gfarm_error_t
gfm_client_process_alloc(struct gfm_connection *gfm_server,
	gfarm_int32_t keytype, const char *sharedkey, size_t sharedkey_size,
	gfarm_pid_t *pidp)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_PROCESS_ALLOC, "ib/l",
	    keytype, sharedkey_size, sharedkey, pidp));
}

#ifdef NOT_USED
gfarm_error_t
gfm_client_process_alloc_child(struct gfm_connection *gfm_server,
	gfarm_int32_t parent_keytype, const char *parent_sharedkey,
	size_t parent_sharedkey_size, gfarm_pid_t parent_pid,
	gfarm_int32_t keytype, const char *sharedkey, size_t sharedkey_size,
	gfarm_pid_t *pidp)
{
	return (gfm_client_rpc(gfm_server,
	    GFM_PROTO_PROCESS_ALLOC_CHILD, "iblib/l", parent_keytype,
	    parent_sharedkey_size, parent_sharedkey, parent_pid,
	    keytype, sharedkey_size, sharedkey, pidp));
}
#endif

gfarm_error_t
gfm_client_process_free(struct gfm_connection *gfm_server)
{
	return (gfm_client_rpc(gfm_server, GFM_PROTO_PROCESS_FREE, "/"));
}

#ifndef __KERNEL__      /* gfsd only */

gfarm_error_t
gfm_client_process_set(struct gfm_connection *gfm_server, const char *user,
	gfarm_int32_t keytype, const char *sharedkey, size_t sharedkey_size,
	gfarm_pid_t pid)
{
	gfarm_error_t e;

	if (keytype != GFM_PROTO_PROCESS_KEY_TYPE_SHAREDSECRET ||
	    sharedkey_size != GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET) {
		gflog_error(GFARM_MSG_1000061,
		    "gfm_client_process_set: type=%d, size=%d: "
		    "programming error", (int)keytype, (int)sharedkey_size);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}

	gfm_client_connection_lock(gfm_server);
	e = gfm_client_rpc(gfm_server, GFM_PROTO_PROCESS_SET, "sibl/",
	    user, keytype, sharedkey_size, sharedkey, pid);
	if (e == GFARM_ERR_NO_ERROR) {
		memcpy(gfm_server->pid_key, sharedkey, sharedkey_size);
		gfm_server->pid = pid;
	} else {
		gflog_debug(GFARM_MSG_1001171,
			"gfm_client_rpc() failed: %s",
			gfarm_error_string(e));
	}
	gfm_client_connection_unlock(gfm_server);
	return (e);
}
#endif /* __KERNEL__ */

/*
 * compound request - convenience function
 */

gfarm_error_t
gfm_client_compound_fd_op(struct gfm_connection *gfm_server, gfarm_int32_t fd,
	gfarm_error_t (*request_op)(struct gfm_connection *,
	    struct gfp_xdr_context *, void *),
	gfarm_error_t (*result_op)(struct gfm_connection *,
	    struct gfp_xdr_context *, void *),
	void (*cleanup_op)(struct gfm_connection *, void *),
	void *closure)
{
	gfarm_error_t e;
	struct gfp_xdr_context *ctx;

	gfm_client_connection_lock(gfm_server);
	if ((e = gfp_xdr_context_alloc(gfm_server->conn, &ctx)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_warning(GFARM_MSG_UNFIXED, "gfp_xdr_context_alloc: %s",
		    gfarm_error_string(e));
		return (e);
	}
	
	if ((e = gfm_client_compound_begin_request(gfm_server, ctx))
	    != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000062, "compound_begin request: %s",
		    gfarm_error_string(e));
	else if ((e = gfm_client_put_fd_request(gfm_server, ctx, fd))
	    != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000063, "put_fd request: %s",
		    gfarm_error_string(e));
	else if ((e = (*request_op)(gfm_server, ctx, closure))
	    != GFARM_ERR_NO_ERROR)
		;
	else if ((e = gfm_client_compound_end_request(gfm_server, ctx))
	    != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000064, "compound_end request: %s",
		    gfarm_error_string(e));

	else if ((e = gfm_client_compound_begin_result(gfm_server, ctx))
	    != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000065, "compound_begin result: %s",
		    gfarm_error_string(e));
	else if ((e = gfm_client_put_fd_result(gfm_server, ctx))
	    != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000066, "put_fd result: %s",
		    gfarm_error_string(e));
	else if ((e = (*result_op)(gfm_server, ctx, closure))
	    != GFARM_ERR_NO_ERROR)
		;
	else if ((e = gfm_client_compound_end_result(gfm_server, ctx))
	    != GFARM_ERR_NO_ERROR) {
		gflog_warning(GFARM_MSG_1000067, "compound_end result: %s",
		    gfarm_error_string(e));
		if (cleanup_op != NULL)
			(*cleanup_op)(gfm_server, closure);
	}
	gfm_client_connection_unlock(gfm_server);

	gfp_xdr_context_free(gfm_server->conn, ctx);

	return (e);
}

/**
 * metadb_server management
 */

static gfarm_error_t
gfm_client_metadb_server_get_n(struct gfm_connection *gfm_server,
	size_t *sizep,
	int n, struct gfarm_metadb_server *mss)
{
	gfarm_error_t e;
	struct gfarm_metadb_server *ms;
	int i;

	for (i = 0; i < n; ++i) {
		ms = &mss[i];
		e = gfm_client_xdr_recv(gfm_server, sizep, "sisii",
		    &ms->name, &ms->port, &ms->clustername, &ms->flags,
		    &ms->tflags);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
			    "gfm_client_xdr_recv() failed: %s",
			    gfarm_error_string(e));
			return (e);
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfm_client_metadb_server_get_alloc_n(struct gfm_connection *gfm_server,
	struct gfp_xdr_xid_record *xidr, size_t size, int n,
	int *np, struct gfarm_metadb_server **mssp, const char *diag)
{
	gfarm_error_t e, e2;
	struct gfarm_metadb_server *mss;

	if (n == 0) {
		*mssp = NULL;
		*np = 0;
		return (GFARM_ERR_NO_ERROR);
	}

	GFARM_MALLOC_ARRAY(mss, n);
	if (mss == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002577,
		    "alloc metadb_server %d: %s", n, gfarm_error_string(e));
	} else if ((e = gfm_client_metadb_server_get_n(gfm_server, &size,
	    n, mss)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002578,
		    "gfm_client_metadb_server_get_n() failed: %s",
		    gfarm_error_string(e));
	}
	e2 = gfm_client_rpc_raw_result_end(gfm_server, xidr, size);
	if (e != GFARM_ERR_NO_ERROR) {
		return (e);
	} else if (e2 != GFARM_ERR_NO_ERROR ) {
		/* XXX mss memory leak */
		return (e2);
	}
	*np = n;
	*mssp = mss;
	return (GFARM_ERR_NO_ERROR);
}

/* called by gftool/gfmdhost */
gfarm_error_t
gfm_client_metadb_server_get(struct gfm_connection *gfm_server,
	const char *name, struct gfarm_metadb_server *ms)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	int n;
	struct gfarm_metadb_server *msp;
	static const char diag[] = "gfm_client_metadb_server_get";

	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_METADB_SERVER_GET,
	    "s/i", name, &n)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002579,
		    "gfm_client_rpc() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (n == 0) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		gflog_debug(GFARM_MSG_1002580,
		    "%s: %s", gfarm_error_string(e), name);
	} else {
		e = gfm_client_metadb_server_get_alloc_n(gfm_server,
		    xidr, size, 1, &n, &msp, diag);
		if (e == GFARM_ERR_NO_ERROR) {
			*ms = *msp;
			free(msp);
		}
	}
	return (e);
}

/* called by gftool/gfmdhost */
gfarm_error_t
gfm_client_metadb_server_get_all(struct gfm_connection *gfm_server, int *np,
	struct gfarm_metadb_server **mssp)
{
	gfarm_error_t e;
	struct gfp_xdr_xid_record *xidr;
	size_t size;
	gfarm_int32_t n;
	static const char diag[] = "gfm_client_metadb_server_get_all";

	if ((e = gfm_client_rpc_request_and_result_begin(gfm_server,
	    &xidr, &size, GFM_PROTO_METADB_SERVER_GET_ALL,
	    "/i", &n)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002581,
		    "gfm_client_rpc() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	return (gfm_client_metadb_server_get_alloc_n(gfm_server, xidr, size,
	    n, np, mssp, diag));
}

static gfarm_error_t
gfm_client_metadb_server_send(struct gfm_connection *gfm_server,
	struct gfarm_metadb_server *ms, int op, const char *diag)
{
	gfarm_error_t e;

	if ((e = gfm_client_rpc(gfm_server, op, "sisi/",
	    ms->name, ms->port, ms->clustername, ms->flags))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002582,
		    "%s: gfm_client_rpc() failed: %s",
		    diag, gfarm_error_string(e));
	}
	return (e);
}

gfarm_error_t
gfm_client_metadb_server_set(struct gfm_connection *gfm_server,
	struct gfarm_metadb_server *ms)
{
	return (gfm_client_metadb_server_send(gfm_server, ms,
	    GFM_PROTO_METADB_SERVER_SET, "gfm_client_metadb_server_set"));
}

gfarm_error_t
gfm_client_metadb_server_modify(struct gfm_connection *gfm_server,
	struct gfarm_metadb_server *ms)
{
	return (gfm_client_metadb_server_send(gfm_server, ms,
	    GFM_PROTO_METADB_SERVER_MODIFY, "gfm_client_metadb_server_modify"));
}

gfarm_error_t
gfm_client_metadb_server_remove(struct gfm_connection *gfm_server,
	const char *name)
{
	gfarm_error_t e;

	if ((e = gfm_client_rpc(gfm_server, GFM_PROTO_METADB_SERVER_REMOVE,
	    "s/", name)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002583,
		    "gfm_client_rpc() failed: %s",
		    gfarm_error_string(e));
	}
	return (e);
}


#if 0 /* not used in gfarm v2 */
/*
 * job management
 */

gfarm_error_t
gfj_client_lock_register(struct gfm_connection *gfm_server)
{
	return (gfm_client_rpc(gfm_server, 0, GFJ_PROTO_LOCK_REGISTER,
			       "/"));
}

gfarm_error_t
gfj_client_unlock_register(struct gfm_connection *gfm_server)
{
	return (gfm_client_rpc(gfm_server, 0, GFJ_PROTO_UNLOCK_REGISTER, "/"));
}

gfarm_error_t
gfj_client_register(struct gfm_connection *gfm_server,
		    struct gfarm_job_info *job, int flags,
		    int *job_idp)
{
	gfarm_error_t e;
	int i;
	gfarm_int32_t job_id;

	e = gfm_client_rpc_request(gfm_server, GFJ_PROTO_REGISTER,
				   "iisssi",
				   (gfarm_int32_t)flags,
				   (gfarm_int32_t)job->total_nodes,
				   job->job_type,
				   job->originate_host,
				   job->gfarm_url_for_scheduling,
				   (gfarm_int32_t)job->argc);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	for (i = 0; i < job->argc; i++)
		e = gfp_xdr_send(gfm_server->conn, "s", job->argv[i]);
	for (i = 0; i < job->total_nodes; i++)
		e = gfp_xdr_send(gfm_server->conn, "s",
				   job->nodes[i].hostname);
	e = gfm_client_rpc_result(gfm_server, 0, "i", &job_id);
	if (e == GFARM_ERR_NO_ERROR)
		*job_idp = job_id;
	return (e);
}

gfarm_error_t
gfj_client_unregister(struct gfm_connection *gfm_server, int job_id)
{
	return (gfm_client_rpc(gfm_server, 0, GFJ_PROTO_UNREGISTER,
	    "i/", job_id));
}

gfarm_error_t
gfj_client_list(struct gfm_connection *gfm_server, char *user,
		      int *np, int **jobsp)
{
	gfarm_error_t e;
	int i, n, eof, *jobs;
	gfarm_int32_t job_id;

	e = gfm_client_rpc(gfm_server, 0, GFJ_PROTO_LIST, "s/i", user, &n);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	GFARM_MALLOC_ARRAY(jobs, n);
	if (jobs == NULL)
		return (GFARM_ERR_NO_MEMORY);
	for (i = 0; i < n; i++) {
		e = gfp_xdr_recv(gfm_server->conn, 0, &eof, "i", &job_id);
		if (e != GFARM_ERR_NO_ERROR) {
			free(jobs);
			return (e);
		}
		if (eof) {
			free(jobs);
			return (GFARM_ERR_PROTOCOL);
		}
		jobs[i] = job_id;
	}
	*np = n;
	*jobsp = jobs;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfj_client_info_entry(struct gfp_xdr *conn,
		      struct gfarm_job_info *info)
{
	gfarm_error_t e;
	int eof, i;
	gfarm_int32_t total_nodes, argc, node_pid, node_state;

	e = gfp_xdr_recv(conn, 0, &eof, "issssi",
			   &total_nodes,
			   &info->user,
			   &info->job_type,
			   &info->originate_host,
			   &info->gfarm_url_for_scheduling,
			   &argc);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (eof)
		return (GFARM_ERR_PROTOCOL);
	GFARM_MALLOC_ARRAY(info->argv, argc + 1);
	GFARM_MALLOC_ARRAY(info->nodes, total_nodes);
	if (info->argv == NULL || info->nodes == NULL) {
		free(info->job_type);
		free(info->originate_host);
		free(info->gfarm_url_for_scheduling);
		if (info->argv != NULL)
			free(info->argv);
		if (info->nodes != NULL)
			free(info->nodes);
		return (GFARM_ERR_NO_MEMORY);
	}

	for (i = 0; i < argc; i++) {
		e = gfp_xdr_recv(conn, 0, &eof, "s", &info->argv[i]);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			if (e == GFARM_ERR_NO_ERROR)
				e = GFARM_ERR_PROTOCOL;
			while (--i >= 0)
				free(info->argv[i]);
			free(info->job_type);
			free(info->originate_host);
			free(info->gfarm_url_for_scheduling);
			free(info->argv);
			free(info->nodes);
			return (e);
		}
	}
	info->argv[argc] = NULL;

	for (i = 0; i < total_nodes; i++) {
		e = gfp_xdr_recv(conn, 0, &eof, "sii",
				   &info->nodes[i].hostname,
				   &node_pid, &node_state);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			if (e == GFARM_ERR_NO_ERROR)
				e = GFARM_ERR_PROTOCOL;
			while (--i >= 0)
				free(info->nodes[i].hostname);
			for (i = 0; i < argc; i++)
				free(info->argv[i]);
			free(info->job_type);
			free(info->originate_host);
			free(info->gfarm_url_for_scheduling);
			free(info->argv);
			free(info->nodes);
			return (e);
		}
		info->nodes[i].pid = node_pid;
		info->nodes[i].state = node_state;
	}
	info->total_nodes = total_nodes;
	info->argc = argc;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfj_client_info(struct gfm_connection *gfm_server, int n, int *jobs,
		      struct gfarm_job_info *infos)
{
	gfarm_error_t e;
	int i;

	e = gfm_client_rpc_request(gfm_server, GFJ_PROTO_INFO, "i", n);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	for (i = 0; i < n; i++) {
		e = gfp_xdr_send(gfm_server->conn, "i",
				   (gfarm_int32_t)jobs[i]);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	}

	gfarm_job_info_clear(infos, n);
	for (i = 0; i < n; i++) {
		e = gfm_client_rpc_result(gfm_server, 0, "");
		if (e == GFARM_ERR_NO_SUCH_OBJECT)
			continue;
		if (e == GFARM_ERR_NO_ERROR)
			e = gfj_client_info_entry(gfm_server->conn, &infos[i]);
		if (e != GFARM_ERR_NO_ERROR) {
			gfarm_job_info_free_contents(infos, i - 1);
			return (e);
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

void
gfarm_job_info_clear(struct gfarm_job_info *infos, int n)
{
	memset(infos, 0, sizeof(struct gfarm_job_info) * n);
}

void
gfarm_job_info_free_contents(struct gfarm_job_info *infos, int n)
{
	int i, j;
	struct gfarm_job_info *info;

	for (i = 0; i < n; i++) {
		info = &infos[i];
		if (info->user == NULL) /* this entry is not valid */
			continue;
		free(info->user);
		free(info->job_type);
		free(info->originate_host);
		free(info->gfarm_url_for_scheduling);
		for (j = 0; j < info->argc; j++)
			free(info->argv[j]);
		free(info->argv);
		for (j = 0; j < info->total_nodes; j++)
			free(info->nodes[j].hostname);
		free(info->nodes);
	}
}

/*
 * job management - convenience function
 */
gfarm_error_t
gfarm_user_job_register(struct gfm_connection *gfm_server,
			int nusers, char **users,
			char *job_type, char *sched_file,
			int argc, char **argv,
			int *job_idp)
{
	gfarm_error_t e;
	int i, p;
	struct gfarm_job_info job_info;

	gfarm_job_info_clear(&job_info, 1);
	job_info.total_nodes = nusers;
	job_info.user = gfm_client_username(gfm_server);
	job_info.job_type = job_type;
	e = gfm_host_get_canonical_self_name(gfm_server,
	    &job_info.originate_host, &p);
	if (e == GFARM_ERR_UNKNOWN_HOST) {
		/*
		 * gfarm client doesn't have to be a compute user,
		 * so, we should allow non canonical name here.
		 */
		job_info.originate_host = gfarm_host_get_self_name();
	} else if (e != GFARM_ERR_NO_ERROR)
		return (e);
	job_info.gfarm_url_for_scheduling = sched_file;
	job_info.argc = argc;
	job_info.argv = argv;
	GFARM_MALLOC_ARRAY(job_info.nodes, nusers);
	if (job_info.nodes == NULL)
		return (GFARM_ERR_NO_MEMORY);
	for (i = 0; i < nusers; i++) {
		e = gfm_host_get_canonical_name(gfm_server, users[i],
		    &job_info.nodes[i].hostname, &p);
		if (e != GFARM_ERR_NO_ERROR) {
			while (--i >= 0)
				free(job_info.nodes[i].hostname);
			free(job_info.nodes);
			return (e);
		}
	}
	e = gfj_client_register(gfm_server, &job_info, 0, job_idp);
	for (i = 0; i < nusers; i++)
		free(job_info.nodes[i].hostname);
	free(job_info.nodes);
	return (e);
}
#endif
