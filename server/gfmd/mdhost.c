/*
 * $Id$
 */

#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "hash.h"
#include "thrsubr.h"

#include "context.h"
#include "auth.h"
#include "gfp_xdr.h"
#include "config.h"

#include "gfm_client.h"
#include "gfm_proto.h"
#include "filesystem.h"

#include "gfmd.h"
#include "subr.h"
#include "rpcsubr.h"
#include "host.h"
#include "user.h"
#include "peer.h"
#include "local_peer.h"
#include "abstract_host.h"
#include "abstract_host_impl.h"
#include "netsendq.h"
#include "metadb_server.h"
#include "mdhost.h"
#include "mdcluster.h"
#include "journal_file.h"
#include "db_access.h"
#include "db_journal.h"
#include "gfmd_channel.h"
#include "relay.h"

/* in-core gfarm_metadb_server */
struct mdhost {
	struct abstract_host ah; /* must be the first member of this struct */

	struct gfarm_metadb_server ms;
	pthread_mutex_t mutex;
	enum mdhost_seqnum_state {
		seqnum_state_unknown,
		seqnum_state_ok,
		seqnum_state_out_of_sync,
		seqnum_state_error
	} seqnum_state;
	struct mdcluster *cluster;
};
static const char MDHOST_MUTEX_DIAG[]		= "mdhost_mutex";

/* global mutex is for mdhost_hashtab */
pthread_mutex_t mdhost_global_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char MDHOST_GLOBAL_MUTEX_DIAG[]	= "mdhost_global_mutex";
static struct gfarm_hash_table *mdhost_hashtab;

/* no need to protect since it is only updated in mdhost_init() */
static struct mdhost *mdhost_self;

pthread_mutex_t mdhost_master_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char MDHOST_MASTER_MUTEX_DIAG[]	= "mdhost_master_mutex";
static struct mdhost *mdhost_master;

struct netsendq_manager *mdhost_send_manager;

#define MDHOST_HASHTAB_SIZE	31

#define FOREACH_MDHOST(it) \
	for (gfarm_hash_iterator_begin(mdhost_hashtab, &(it)); \
	     !gfarm_hash_iterator_is_end(&(it)); \
	     gfarm_hash_iterator_next(&(it)))

static gfarm_error_t mdhost_updated(void);

/*
 **********************************************************************
 * for gfmd_channel.c
 */
static void (*mdhost_update_hook_for_journal_send)(void);

void
mdhost_set_update_hook_for_journal_send(void (*hook)(void))
{
	mdhost_update_hook_for_journal_send = hook;
}

/**********************************************************************/

static void
mdhost_mutex_lock(struct mdhost *m, const char *diag)
{
	gfarm_mutex_lock(&m->mutex, diag, MDHOST_MUTEX_DIAG);
}

static void
mdhost_mutex_unlock(struct mdhost *m, const char *diag)
{
	gfarm_mutex_unlock(&m->mutex, diag, MDHOST_MUTEX_DIAG);
}

static void
mdhost_global_mutex_lock(const char *diag)
{
	gfarm_mutex_lock(&mdhost_global_mutex, diag, MDHOST_GLOBAL_MUTEX_DIAG);
}

static void
mdhost_global_mutex_unlock(const char *diag)
{
	gfarm_mutex_unlock(&mdhost_global_mutex, diag,
		MDHOST_GLOBAL_MUTEX_DIAG);
}

static void
mdhost_master_mutex_lock(const char *diag)
{
	gfarm_mutex_lock(&mdhost_master_mutex, diag, MDHOST_MASTER_MUTEX_DIAG);
}

static void
mdhost_master_mutex_unlock(const char *diag)
{
	gfarm_mutex_unlock(&mdhost_master_mutex, diag,
		MDHOST_MASTER_MUTEX_DIAG);
}

const char *
mdhost_get_name(struct mdhost *m)
{
	return (gfarm_metadb_server_get_name(&m->ms));
}

int
mdhost_is_master(struct mdhost *m)
{
	int is_master;
	static const char diag[] = "mdhost_is_master";

	mdhost_mutex_lock(m, diag);
	is_master = gfarm_metadb_server_is_master(&m->ms);
	mdhost_mutex_unlock(m, diag);
	if (gfarm_get_metadb_server_force_slave() && m == mdhost_lookup_self())
		is_master = 0;
	return (is_master);
}

void
mdhost_set_is_master(struct mdhost *m, int enable)
{
	static const char diag[] = "mdhost_set_is_master";

	mdhost_mutex_lock(m, diag);
	gfarm_metadb_server_set_is_master(&m->ms, enable);
	mdhost_mutex_unlock(m, diag);
	if (enable) {
		mdhost_master_mutex_lock(diag);
		mdhost_master = m;
		mdhost_master_mutex_unlock(diag);
	}
}

int
mdhost_is_self(struct mdhost *m)
{
	return (gfarm_metadb_server_is_self(&m->ms));
}

struct abstract_host *
mdhost_to_abstract_host(struct mdhost *m)
{
	return (&m->ah);
}

static struct host *
mdhost_downcast_to_host(struct abstract_host *h)
{
	gflog_fatal(GFARM_MSG_1002925, "downcasting mdhost %p to host", h);
	return (NULL);
}

static struct mdhost *
mdhost_downcast_to_mdhost(struct abstract_host *h)
{
	return ((struct mdhost *)h);
}

static const char *
mdhost_name0(struct abstract_host *h)
{
	return (mdhost_get_name(abstract_host_to_mdhost(h)));
}

int
mdhost_get_port(struct mdhost *m)
{
	return (gfarm_metadb_server_get_port(&m->ms));
}

static int
mdhost_port0(struct abstract_host *h)
{
	return (mdhost_get_port(abstract_host_to_mdhost(h)));
}

int
mdhost_is_up(struct mdhost *m)
{
	return (abstract_host_is_up(mdhost_to_abstract_host(m)));
}

/*
 * The use of this function is only allowed against self node.
 * Because using this against remote node breaks the following invariant:
 * 	if (m->ah.is_active) assert(m->ah.peer != NULL);
 * The "is_active" flag for remote nodes can be set
 * via abstract_host_set_peer() via mdhost_set_peer().
 */
void
mdhost_activate(struct mdhost *m)
{
	abstract_host_activate(mdhost_to_abstract_host(m), MDHOST_MUTEX_DIAG);
}

static int
mdhost_is_valid(struct mdhost *m)
{
	return (abstract_host_is_valid(mdhost_to_abstract_host(m),
	    MDHOST_MUTEX_DIAG));
}

void
mdhost_set_peer(struct mdhost *m, struct peer *peer, int version)
{
	abstract_host_set_peer(mdhost_to_abstract_host(m), peer, version);
}

static struct mdhost *
mdhost_iterator_access(struct gfarm_hash_iterator *it)
{
	return (*(struct mdhost **)gfarm_hash_entry_data(
	    gfarm_hash_iterator_access(it)));
}

void
mdhost_foreach(int (*func)(struct mdhost *, void *), void *closure)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m;
	struct mdhost *self = mdhost_lookup_self();
	static const char diag[] = "mdhost_foreach";

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (mdhost_is_valid(m) && m != self && func(m, closure) == 0)
			break;
	}
	mdhost_global_mutex_unlock(diag);
}

static int
mdhost_is_default_master(struct mdhost *m)
{
	int is_default_master;
	static const char diag[] = "mdhost_is_default_master";

	mdhost_mutex_lock(m, diag);
	is_default_master = gfarm_metadb_server_is_default_master(&m->ms);
	mdhost_mutex_unlock(m, diag);
	return (is_default_master);
}

static void
mdhost_set_is_default_master(struct mdhost *m, int enable)
{
	static const char diag[] = "mdhost_set_is_default_master";

	mdhost_mutex_lock(m, diag);
	gfarm_metadb_server_set_is_default_master(&m->ms, enable);
	mdhost_mutex_unlock(m, diag);
}

struct mdcluster *
mdhost_get_cluster(struct mdhost *m)
{
	return (m->cluster);
}

void
mdhost_set_cluster(struct mdhost *m, struct mdcluster *c)
{
	m->cluster = c;
}

const char *
mdhost_get_cluster_name(struct mdhost *m)
{
	return (m->ms.clustername);
}

static enum mdhost_seqnum_state
mdhost_get_seqnum_state(struct mdhost *m)
{
	enum mdhost_seqnum_state s;
	static const char diag[] = "mdhost_get_seqnum_state";

	if (mdhost_is_master(m))
		return (seqnum_state_ok);

	mdhost_mutex_lock(m, diag);
	s = m->seqnum_state;
	mdhost_mutex_unlock(m, diag);
	return (s);
}

static void
mdhost_set_seqnum_state(struct mdhost *m, enum mdhost_seqnum_state s)
{
	static const char diag[] = "mdhost_set_seqnum_state";

	mdhost_mutex_lock(m, diag);
	m->seqnum_state = s;
	mdhost_mutex_unlock(m, diag);
}

void
mdhost_set_seqnum_unknown(struct mdhost *m)
{
	mdhost_set_seqnum_state(m, seqnum_state_unknown);
}

void
mdhost_set_seqnum_ok(struct mdhost *m)
{
	mdhost_set_seqnum_state(m, seqnum_state_ok);
}

void
mdhost_set_seqnum_out_of_sync(struct mdhost *m)
{
	mdhost_set_seqnum_state(m, seqnum_state_out_of_sync);
}

void
mdhost_set_seqnum_error(struct mdhost *m)
{
	mdhost_set_seqnum_state(m, seqnum_state_error);
}

void
mdhost_set_seqnum_state_by_error(struct mdhost *m, gfarm_error_t e)
{
	switch (e) {
	case GFARM_ERR_EXPIRED:
		mdhost_set_seqnum_out_of_sync(m);
		break;
	default:
		mdhost_set_seqnum_error(m);
		break;
	}
}

static void
mdhost_invalidate(struct mdhost *m)
{
	abstract_host_invalidate(mdhost_to_abstract_host(m));
}

int
mdhost_self_is_master_candidate(void)
{
	return (gfarm_metadb_server_is_master_candidate(
		&mdhost_lookup_self()->ms));
}

static void
mdhost_validate(struct mdhost *m)
{
	abstract_host_validate(mdhost_to_abstract_host(m));
}

static void
mdhost_set_peer_locked(struct abstract_host *h, struct peer *peer)
{
}

static void
mdhost_set_peer_unlocked(struct abstract_host *h, struct peer *peer)
{
	mdhost_set_seqnum_unknown(abstract_host_to_mdhost(h));
}

static void
mdhost_unset_peer(struct abstract_host *h, struct peer *peer)
{
}

/*
 * if mdhost_get_peer() is called,
 * same number of mdhost_put_peer() calls should be made.
 */
struct peer *
#ifdef PEER_REFCOUNT_DEBUG
mdhost_get_peer_impl(struct mdhost *mh,
	const char *file, int line, const char *func)
#else
mdhost_get_peer(struct mdhost *mh)
#endif
{
	static const char diag[] = "mdhost_get_peer";

#ifdef PEER_REFCOUNT_DEBUG
	struct peer *peer = abstract_host_get_peer(&mh->ah, diag);

	gflog_info(GFARM_MSG_1003650, "%s(%d):%s(): mdhost_get_peer():%p",
	    file, line, func, peer);
	return (peer);
#else
	return (abstract_host_get_peer(&mh->ah, diag));
#endif
}

void
#ifdef PEER_REFCOUNT_DEBUG
mdhost_put_peer_impl(struct mdhost *mh, struct peer *peer,
	const char *file, int line, const char *func)
#else
mdhost_put_peer(struct mdhost *mh, struct peer *peer)
#endif
{
	abstract_host_put_peer(&mh->ah, peer);
#ifdef PEER_REFCOUNT_DEBUG
	gflog_info(GFARM_MSG_1003651, "%s(%d):%s(): mdhost_put_peer(%p)",
	    file, line, func, peer);
#endif
}

static void
mdhost_disable(struct abstract_host *h)
{
}

static void
mdhost_disabled(struct abstract_host *h, struct peer *peer)
{
}

struct abstract_host_ops mdhost_ops = {
	mdhost_downcast_to_host,
	mdhost_downcast_to_mdhost,
	mdhost_name0,
	mdhost_port0,
	mdhost_set_peer_locked,
	mdhost_set_peer_unlocked,
	mdhost_unset_peer,
	mdhost_disable,
	mdhost_disabled,
};

const struct netsendq_type *
	mdhost_queue_types[NETSENDQ_TYPE_GFM_PROTO_NUM_TYPES] = {
};

static struct mdhost *
mdhost_new(struct gfarm_metadb_server *ms)
{
	gfarm_error_t e;
	struct mdhost *m;
	static const char diag[] = "mdhost_new";

	if ((m = malloc(sizeof(struct mdhost))) == NULL) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "mdhost %s: no memory", gfarm_metadb_server_get_name(ms));
		return (NULL);
	}

	e = abstract_host_init(&m->ah, &mdhost_ops, mdhost_send_manager, diag);
	if (e != GFARM_ERR_NO_ERROR) {
		free(m);
		gflog_error(GFARM_MSG_UNFIXED,
		    "mdhost %s: %s",
		    gfarm_metadb_server_get_name(ms), gfarm_error_string(e));
		return (NULL);
	}
	m->ms = *ms;
	gfarm_mutex_init(&m->mutex, diag, MDHOST_MUTEX_DIAG);
	mdhost_validate(m);
	mdhost_set_seqnum_unknown(m);
	m->cluster = NULL;

	return (m);
}

static struct mdhost *
mdhost_lookup_internal(const char *hostname)
{
	struct gfarm_hash_entry *entry;
	static const char diag[] = "mdhost_lookup_internal";

	mdhost_global_mutex_lock(diag);
	entry = gfarm_hash_lookup(mdhost_hashtab, &hostname,
		sizeof(hostname));
	mdhost_global_mutex_unlock(diag);
	if (entry == NULL)
		return (NULL);
	return (*(struct mdhost **)gfarm_hash_entry_data(entry));
}

struct mdhost *
mdhost_lookup(const char *hostname)
{
	struct mdhost *m = mdhost_lookup_internal(hostname);

	return (m && mdhost_is_valid(m) ? m : NULL);
}

struct mdhost *
mdhost_lookup_metadb_server(struct gfarm_metadb_server *ms)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m, *mm = NULL;
	static const char diag[] = "mdhost_lookup_metadb_server";

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (mdhost_is_valid(m) && &m->ms == ms) {
			mm = m;
			break;
		}
	}
	mdhost_global_mutex_unlock(diag);
	return (mm);
}

struct mdhost *
mdhost_lookup_master(void)
{
	struct mdhost *m;
	static const char diag[] = "mdhost_lookup_master";

	mdhost_master_mutex_lock(diag);
	m = mdhost_master;
	mdhost_master_mutex_unlock(diag);
	if (m == NULL)
		gflog_fatal(GFARM_MSG_1003324, "%s: no master, abort", diag);
	return (m);
}


struct mdhost *
mdhost_lookup_self(void)
{
	return (mdhost_self);
}

int
mdhost_self_is_master(void)
{
	struct mdhost *m = mdhost_lookup_self();

	return (mdhost_is_master(m));
}

/*
 * PREREQUISITE: giant_lock
 *
 * peer may be NULL.
 */
void
mdhost_disconnect_request(struct mdhost *m, struct peer *peer)
{
	static const char diag[] = "mdhost_disconnect_request";

	return (abstract_host_disconnect_request(&m->ah, peer, diag));
}

/* PREREQUISITE: giant_lock */
void
mdhost_set_self_as_master(void)
{
	struct mdhost *m, *s = mdhost_lookup_self();

	/* stop "force_slave" mode at first */
	gfarm_set_metadb_server_force_slave(0);

	m = mdhost_lookup_master();
	if (m == NULL) {
		gflog_error(GFARM_MSG_1003325,
		    "mdhost_set_self_as_master: no master");
		return;
	}
	mdhost_disconnect_request(m, NULL);
	mdhost_set_is_master(m, 0);
	mdhost_set_is_master(s, 1);
}

gfarm_error_t
mdhost_enter(struct gfarm_metadb_server *ms, struct mdhost **mpp)
{
	struct gfarm_hash_entry *entry;
	int created;
	struct mdhost *mh;
	gfarm_error_t e;

	mh = mdhost_lookup_internal(ms->name);
	if (mh) {
		if (mdhost_is_valid(mh))
			return (GFARM_ERR_ALREADY_EXISTS);

		mdhost_validate(mh);
		if (mpp)
			*mpp = mh;
		/* copy ms to mh except name */
		free(ms->name);
		ms->name = mh->ms.name;
		if (gfarm_get_metadb_replication_enabled())
			free(mh->ms.clustername);
		mh->ms = *ms;
		gfarm_metadb_server_set_is_memory_owned_by_fs(&mh->ms, 0);
		return (GFARM_ERR_NO_ERROR);
	}

	mh = mdhost_new(ms);
	if (mh == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002928,
		    "%s", gfarm_error_string(e));
		return (e);
	}
	if (gfarm_get_metadb_replication_enabled() &&
	    (e = mdcluster_get_or_create_by_mdhost(mh)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002929,
		    "%s", gfarm_error_string(e));
		free(mh);
		return (e);
	}
	entry = gfarm_hash_enter(mdhost_hashtab,
	    &mh->ms.name, sizeof(mh->ms.name),
	    sizeof(struct mdhost *), &created);
	if (entry == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002930,
		    "%s", gfarm_error_string(e));
		free(mh);
		return (e);
	}
	if (!created) {
		gflog_debug(GFARM_MSG_1002931,
		    "Entry %s already exists", ms->name);
		free(mh);
		return (GFARM_ERR_ALREADY_EXISTS);
	}
	*(struct mdhost **)gfarm_hash_entry_data(entry) = mh;

	if (mpp)
		*mpp = mh;
	return (GFARM_ERR_NO_ERROR);
}

int
mdhost_is_sync_replication(struct mdhost *mh)
{
	struct mdhost *mmh = mdhost_lookup_master();

	assert(mh != mmh);
	return (mh->cluster == mmh->cluster);
}

int
mdhost_get_flags(struct mdhost *mh)
{
	return (mh->ms.flags);
}

int
mdhost_has_async_replication_target(void)
{
	struct gfarm_hash_iterator it;
	struct mdhost *mh;
	struct mdhost *mmh = mdhost_lookup_master();
	static const char diag[] = "mdhost_has_async_replication_target";
	int ret = 0;

	if (mdhost_get_count() == 1)
		return (ret);
	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (mdhost_is_valid(mh) && mh != mmh &&
		    mh->cluster != mmh->cluster) {
			ret = 1;
			break;
		}
	}
	mdhost_global_mutex_unlock(diag);
	return (ret);
}

void
mdhost_add_one(void *closure, struct gfarm_metadb_server *ms)
{
	gfarm_error_t e = mdhost_enter(ms, NULL);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1002932,
		    "mdhost_add_one: %s", gfarm_error_string(e));
}

gfarm_error_t
mdhost_modify_in_cache(struct mdhost *mh, struct gfarm_metadb_server *ms)
{
	int cluster_changed = strcmp(mh->ms.clustername, ms->clustername) != 0;
	static const char diag[] = "mdhost_modify_in_cache";

	if (cluster_changed)
		mdcluster_remove_mdhost(mh);
	free(mh->ms.clustername);
	mh->ms.clustername = strdup_ck(ms->clustername, diag);
	mh->ms.port = ms->port;
	mh->ms.flags = ms->flags;
	if (cluster_changed)
		return (mdcluster_get_or_create_by_mdhost(mh));
	return (GFARM_ERR_NO_ERROR);
}

/* PREREQUISITE: giant_lock */
gfarm_error_t
mdhost_remove_in_cache(const char *name)
{
	struct mdhost *m;
	struct peer *peer;
	gfarm_error_t e;

	m = mdhost_lookup(name);
	if (m == NULL) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		gflog_debug(GFARM_MSG_1002933,
		    "%s: %s", gfarm_error_string(e), name);
		return (e);
	}
	if ((peer = mdhost_get_peer(m)) != NULL) { /* increment refcount */
		mdhost_disconnect_request(m, peer);
		mdhost_put_peer(m, peer); /* decrement refcount */
	}
	mdcluster_remove_mdhost(m);
	mdhost_invalidate(m);

	if ((e = mdhost_updated()) != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1003434,
		    "%s", gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
metadb_server_reply(struct peer *peer, struct mdhost *m)
{
	struct gfarm_metadb_server *ms, tms;
	struct gfp_xdr *xdr = peer_get_conn(peer);

	ms = &m->ms;
	tms.tflags = ms->tflags;
	if (!mdhost_is_master(m) && mdhost_is_sync_replication(m))
		gfarm_metadb_server_set_is_sync_replication(&tms, 1);
	if (mdhost_is_up(m))
		gfarm_metadb_server_set_is_active(&tms, 1);

	switch (mdhost_get_seqnum_state(m)) {
	case seqnum_state_unknown:
		gfarm_metadb_server_set_seqnum_is_unknown(&tms);
		break;
	case seqnum_state_ok:
		gfarm_metadb_server_set_seqnum_is_ok(&tms);
		break;
	case seqnum_state_out_of_sync:
		gfarm_metadb_server_set_seqnum_is_out_of_sync(&tms);
		break;
	case seqnum_state_error:
		gfarm_metadb_server_set_seqnum_is_error(&tms);
		break;
	}

	return (gfp_xdr_send(xdr, "sisii",
	    ms->name, ms->port, ms->clustername ? ms->clustername : "",
	    ms->flags, tms.tflags));
}

int
mdhost_get_count(void)
{
	struct gfarm_hash_iterator it;
	int n = 0;
	static const char diag[] = "mdhost_get_count";

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		if (mdhost_is_valid(mdhost_iterator_access(&it)))
			++n;
	}
	mdhost_global_mutex_unlock(diag);
	return (n);
}

static gfarm_error_t
metadb_server_get(struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int (*match_op)(struct mdhost *, void *), void *closure,
	const char *diag)
{
	struct peer *mhpeer;
	gfarm_error_t e, e2;
	int size_pos;
	gfarm_int32_t nhosts, nmatch, i;
	struct gfarm_hash_iterator it;
	struct mdhost *mh, **match;

	giant_lock();
	nhosts = mdhost_get_count();
	assert(nhosts > 0); /* self host must be exist */

	GFARM_MALLOC_ARRAY(match, nhosts);
	nmatch = 0;

	if (match == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002934,
		    "%s", gfarm_error_string(e));
	} else {
		i = 0;
		mdhost_global_mutex_lock(diag);
		FOREACH_MDHOST(it) {
			if (i >= nhosts) /* always false due to giant_lock */
				break;
			mh = mdhost_iterator_access(&it);
			if (mdhost_is_valid(mh) && match_op(mh, closure))
				match[i++] = mh;
		}
		mdhost_global_mutex_unlock(diag);
		nmatch = i;
		e = GFARM_ERR_NO_ERROR;
	}
	/* XXXRELAY FIXME, reply size is not correct */
	e2 = gfm_server_put_reply_begin(peer, &mhpeer, xid, &size_pos, diag,
	    e, "i", nmatch);
	/* if network error doesn't happen, e2 == e here */
	if (e2 == GFARM_ERR_NO_ERROR) {
		i = 0;
		for (i = 0; i < nmatch; ++i) {
			mh = match[i];
			/* XXXRELAY FIXME */
			if ((e2 = metadb_server_reply(peer, mh))
			    != GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1002936,
				    "%s: metadb_server_reply: %s",
				    diag, gfarm_error_string(e));
				break;
			}
		}
		gfm_server_put_reply_end(peer, mhpeer, diag, size_pos);
	}
	free(match);
	giant_unlock();

	return (e2);
}

static int
match_all(struct mdhost *mh, void *closure)
{
	return (1);
}

static int
match_hostname(struct mdhost *mh, void *closure)
{
	return (strcmp(mdhost_get_name(mh), (char *)closure) == 0);
}

gfarm_error_t
gfm_server_metadb_server_get(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e_ret, e_rpc;
	char *name = NULL;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_GET";

	e_ret = gfm_server_get_request(peer, sizep, diag, "s", &name);
	if (e_ret != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_server_get_request() failed: %s",
		    gfarm_error_string(e_ret));
		goto end;
	}
	if (skip) {
		e_ret = GFARM_ERR_NO_ERROR;
		goto end;
	}

	if (!gfarm_get_metadb_replication_enabled())
		e_rpc = GFARM_ERR_OPERATION_NOT_PERMITTED;
	else if ((e_rpc = wait_db_update_info(peer, DBUPDATE_HOST, diag))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s: failed to wait for the backend DB to be updated: %s",
		    diag, gfarm_error_string(e_rpc));
	}

	if (e_rpc == GFARM_ERR_NO_ERROR) {
		e_ret = metadb_server_get(peer, xid, sizep, match_hostname,
		    name, diag);
	} else {
		e_ret = gfm_server_put_reply(peer, xid, sizep, diag, e_rpc,
		    "");
		gflog_debug(GFARM_MSG_1002937, "%s: gfm_server_put_reply: %s",
		    diag, gfarm_error_string(e_rpc));
	}

end:
	free(name);
	return (e_ret);
}

#ifdef DEBUG_MDCLUSTER
static int
mdcluster_dump(struct mdcluster *c, void *closure)
{
	gflog_debug(0, "cluster=%s", mdcluster_get_name(c));
	return (1);
}
#endif

gfarm_error_t
gfm_server_metadb_server_get_all(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e_ret, e_rpc;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_GET_ALL";

	e_ret = gfm_server_get_request(peer, sizep, diag, "");
	if (e_ret != GFARM_ERR_NO_ERROR)
		return (e_ret);
	if (skip)
		return (GFARM_ERR_NO_ERROR);
#ifdef DEBUG_CLUSTER
	mdcluster_foreach(mdcluster_dump, NULL);
#endif

	e_rpc = wait_db_update_info(peer, DBUPDATE_HOST, diag);
	if (e_rpc != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s: failed to wait for the backend DB to be updated: %s",
		    diag, gfarm_error_string(e_rpc));
	}

	if (e_rpc == GFARM_ERR_NO_ERROR) {
		e_ret = metadb_server_get(peer, xid, sizep, match_all,
		    NULL, diag);
	} else {
		e_ret = gfm_server_put_reply(peer, xid, sizep, diag, e_rpc,
		    "");
		gflog_debug(GFARM_MSG_UNFIXED, "%s: gfm_server_put_reply: %s",
		    diag, gfarm_error_string(e_rpc));
	}

	return (e_ret);
}

static gfarm_error_t
metadb_server_recv(struct peer *peer, size_t *sizep, int skip, int command,
	struct gfarm_metadb_server *ms,	struct relayed_request **relayp)
{
	gfarm_error_t e;
	static const char diag[] = "metadb_server_recv";

	e = gfm_server_relay_get_request(peer, sizep, skip, relayp, diag,
	    command, "sisi",
	&ms->name, &ms->port, &ms->clustername, &ms->flags);
	return (e);
}

static gfarm_error_t
metadb_server_verify(struct gfarm_metadb_server *ms, const char *diag)
{
	if (ms->name == NULL || strlen(ms->name) == 0) {
		gflog_debug(GFARM_MSG_1002940, "%s: name is empty",
		    diag);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (strlen(ms->name) > GFARM_HOST_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002941, "%s: too long hostname: %s",
		    diag, ms->name);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (ms->clustername &&
	    strlen(ms->clustername) > GFARM_CLUSTER_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002942,
		    "%s: %s: too long clustername: %s",
		    diag, ms->name, ms->clustername);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (ms->port <= 0 || ms->port >= 65536) {
		gflog_debug(GFARM_MSG_1002943,
		    "%s: %s: invalid port number: %d",
		    diag, ms->name, ms->port);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
mdhost_fix_default_master(struct mdhost *new_mmh, const char *diag)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	struct gfarm_hash_iterator it;
	struct mdhost *mh;

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (!mdhost_is_valid(mh))
			continue;
		if (mh == new_mmh || !mdhost_is_default_master(mh))
			continue;
		mdhost_set_is_default_master(mh, 0);
		if ((e = db_mdhost_modify(&mh->ms, 0)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002944,
			    "%s: db_mdhost_modify failed: %s", diag,
			    gfarm_error_string(e));
			break;
		}
	}
	mdhost_global_mutex_unlock(diag);
	return (e);
}

static gfarm_error_t
mdhost_updated(void)
{
	gfarm_error_t e;
	int i, n = mdhost_get_count();
	struct mdhost *mh;
	struct gfarm_metadb_server **mss;
	struct gfarm_filesystem *fs;
	struct gfarm_hash_iterator it;
	static const char diag[] = "mdhost_updated";

	GFARM_MALLOC_ARRAY(mss, n);
	if (mss == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002945,
		    "%s: %s", diag, gfarm_error_string(e));
		return (e);
	}
	i = 0;
	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (mdhost_is_valid(mh) && i < n)
			mss[i++] = &mh->ms;
	}
	mdhost_global_mutex_unlock(diag);
	fs = gfarm_filesystem_get_default();
	gfarm_filesystem_set_metadb_server_list(fs, mss, i);
	free(mss);
	if (mdhost_update_hook_for_journal_send != NULL)
		(*mdhost_update_hook_for_journal_send)();

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
mdhost_db_modify_default_master(struct mdhost *mh,
	struct gfarm_metadb_server *ms, const char *diag)
{
	gfarm_error_t e;

	if ((e = db_begin(diag)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002946, "db_begin failed: %s",
		    gfarm_error_string(e));
	} else if ((e = db_mdhost_modify(ms, 0)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002947, "db_mdhost_modify failed: %s",
		    gfarm_error_string(e));
	} else if ((e = mdhost_fix_default_master(mh, diag))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002948, "db_mdhost_modify failed: %s",
		    gfarm_error_string(e));
	} else if ((e = db_end(diag)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002949, "db_end failed: %s",
		    gfarm_error_string(e));
	}
	return (e);
}

/* PREREQUISITE: giant_lock */
void
mdhost_set_self_as_default_master(void)
{
	gfarm_error_t e;
	struct mdhost *self = mdhost_lookup_self();
	static const char diag[] = "mdhost_set_self_as_default_master";

	mdhost_set_is_default_master(self, 1);
	if ((e = mdhost_db_modify_default_master(self, &self->ms, diag)) !=
	    GFARM_ERR_NO_ERROR)
		;
	else if ((e = mdhost_updated()) != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002950, "%s: mdhost_updated: %s",
		    diag, gfarm_error_string(e));
}

static gfarm_error_t
metadb_server_check_write_access(struct peer *peer, int from_client,
	const char *diag)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	struct user *user = peer_get_user(peer);

	if (!from_client || user == NULL || !user_is_admin(user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002951,
		    "%s: %s", diag, gfarm_error_string(e));
	}
	return (e);
}

gfarm_error_t
gfm_server_metadb_server_set(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	struct gfarm_metadb_server ms;
	struct mdhost *mh;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_SET";

	memset(&ms, 0, sizeof(ms));
	if ((e = metadb_server_recv(peer, sizep, skip,
	    GFM_PROTO_METADB_SERVER_SET, &ms, &relay)) !=
		GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002952,
		    "metadb_server_recv failure: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	if (relay == NULL) /* do not relay RPC to master gfmd */
		giant_lock();

	if (relay != NULL) {
		gfarm_metadb_server_free(&ms);
	} else if (
	    (e = metadb_server_check_write_access(peer, from_client, diag))
		!= GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if (mdhost_lookup(ms.name)) {
		gflog_debug(GFARM_MSG_1002953,
		    "mdhost already exists");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if ((e = metadb_server_verify(&ms, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if ((e = mdhost_enter(&ms, &mh)) != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if (gfarm_metadb_server_is_default_master(&ms)) {
		if ((e = db_begin(diag)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002954,
			    "db_begin failed: %s",
			    gfarm_error_string(e));
		} else if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002955,
			    "db_mdhost_add failed: %s",
			    gfarm_error_string(e));
		} else if ((e = mdhost_fix_default_master(mh, diag))
		    != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002956,
			    "db_mdhost_fix_default_master failed: %s",
			    gfarm_error_string(e));
		} else if ((e = db_end(diag)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002957,
			    "db_end failed: %s",
			    gfarm_error_string(e));
		}
		if (e != GFARM_ERR_NO_ERROR) {
			mdhost_remove_in_cache(ms.name);
			/* do not free after enter */
			ms.name = ms.clustername = NULL;
		}
	} else if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002958,
		    "db_mdhost_add failed: %s",
		    gfarm_error_string(e));
		mdhost_remove_in_cache(ms.name);
		/* do not free after enter */
		ms.name = ms.clustername = NULL;
	}

	if (e == GFARM_ERR_NO_ERROR)
		mdhost_updated();
	else {
		gflog_debug(GFARM_MSG_1002959,
		    "error occurred during process: %s",
		    gfarm_error_string(e));
		gfarm_metadb_server_free(&ms);
	}

	if (relay == NULL) /* do not relay RPC to master gfmd */
		giant_unlock();

	return (gfm_server_relay_put_reply(peer, xid, sizep, relay, diag,
	    &e, ""));
}

gfarm_error_t
gfm_server_metadb_server_modify(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	struct gfarm_metadb_server ms;
	struct mdhost *mh;
	int isdm;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_MODIFY";

	memset(&ms, 0, sizeof(ms));
	if ((e = metadb_server_recv(peer, sizep, skip,
		GFM_PROTO_METADB_SERVER_MODIFY, &ms, &relay))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002960,
		    "metadb_server_recv failure: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	isdm = gfarm_metadb_server_is_default_master(&ms);

	giant_lock();
	if ((e = metadb_server_check_write_access(peer, from_client, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if ((mh = mdhost_lookup(ms.name)) == NULL) {
		gflog_debug(GFARM_MSG_1002961,
		    "mdhost not found: %s", ms.name);
		e = GFARM_ERR_NO_SUCH_OBJECT;
	} else if (mdhost_is_default_master(mh) &&
	    !gfarm_metadb_server_is_default_master(&ms)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002962,
		    "cannot toggle off default master flag directly: %s",
		    ms.name);
	} else if ((e = metadb_server_verify(&ms, diag))
		   != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else {
		mdhost_modify_in_cache(mh, &ms);
		if (isdm) {
			e = mdhost_db_modify_default_master(mh, &ms, diag);
			if (e != GFARM_ERR_NO_ERROR)
				gflog_error(GFARM_MSG_UNFIXED,
				    "mdhost_db_modify_default_master: %s",
				    gfarm_error_string(e));
		} else {
			e = db_mdhost_modify(&ms, 0);
			if (e != GFARM_ERR_NO_ERROR)
				gflog_error(GFARM_MSG_1002963,
				    "db_mdhost_modify failed: %s",
				    gfarm_error_string(e));
		}
		if (e == GFARM_ERR_NO_ERROR)
			mdhost_updated();
	}
	giant_unlock();
	gfarm_metadb_server_free(&ms);
	return (gfm_server_put_reply(peer, xid, sizep, diag, e, ""));
}

gfarm_error_t
gfm_server_metadb_server_remove(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	char *name;
	struct mdhost *mh;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_REMOVE";

	e = gfm_server_relay_get_request(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_METADB_SERVER_REMOVE, "s", &name);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(name);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		free(name);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		giant_lock();
		if ((e = metadb_server_check_write_access(peer, from_client,
			diag)) != GFARM_ERR_NO_ERROR) {
			/* nothing to do */
		} else if ((mh = mdhost_lookup(name)) == NULL) {
			e = GFARM_ERR_NO_SUCH_OBJECT;
			gflog_debug(GFARM_MSG_1002966,
			    "%s: %s: %s", diag, gfarm_error_string(e), name);
		} else if (mh == mdhost_lookup_self()) {
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			gflog_debug(GFARM_MSG_1002967,
			    "%s: cannot remove self host", diag);
		} else if ((e = mdhost_remove_in_cache(name)) !=
			   GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1002968,
			    "%s: mdhost_remove_in_cache(%s) failed: %s",
			    diag, name, gfarm_error_string(e));
		} else if ((e = db_mdhost_remove(name)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1002969,
			    "%s: db_mdhost_remove(%s) failed: %s",
			    diag, name, gfarm_error_string(e));
		}
		giant_unlock();
	}

	free(name);
	return (gfm_server_relay_put_reply(peer, xid, sizep, relay, diag,
	    &e, ""));
}

/* no need to lock here */
void
mdhost_init(void)
{
	struct mdhost *self;
	struct gfarm_metadb_server ms;
	gfarm_error_t e;
	struct mdhost *mh;
	struct gfarm_hash_iterator it;
	static const char diag[] = "mdhost_init";
	char *metadb_server_name = gfarm_ctxp->metadb_server_name;

	if (gfarm_get_metadb_replication_enabled())
		mdcluster_init();

	mdhost_hashtab =
	    gfarm_hash_table_alloc(MDHOST_HASHTAB_SIZE,
		gfarm_hash_strptr, gfarm_hash_key_equal_strptr);
	if (mdhost_hashtab == NULL)
		gflog_fatal(GFARM_MSG_1002970,
		    "%s", gfarm_error_string(GFARM_ERR_NO_MEMORY));

	if (gfarm_get_metadb_replication_enabled()) {
		e = db_mdhost_load(NULL, mdhost_add_one);
		if (e != GFARM_ERR_NO_ERROR && e != GFARM_ERR_NO_SUCH_OBJECT)
			gflog_fatal(GFARM_MSG_1002971,
			    "%s", gfarm_error_string(e));
	}

	mdhost_send_manager = netsendq_manager_new(
	    NETSENDQ_TYPE_GFM_PROTO_NUM_TYPES, mdhost_queue_types,
	    /* XXX FIXME: use different config parameter */
	    gfarm_metadb_thread_pool_size, gfarm_metadb_job_queue_length,
	    "send queue to mdhost");

	if ((self = mdhost_lookup(metadb_server_name)) == NULL) {
		ms.name = strdup_ck(metadb_server_name, diag);
		ms.port = gfmd_port;
		ms.clustername = strdup_ck("", diag);
		ms.flags = 0;
		ms.tflags = 0;
		gfarm_metadb_server_set_is_self(&ms, 1);
		gfarm_metadb_server_set_is_master(&ms, 1);
		gfarm_metadb_server_set_is_master_candidate(&ms, 1);
		gfarm_metadb_server_set_is_default_master(&ms, 1);
		if ((e = mdhost_enter(&ms, &self)) != GFARM_ERR_NO_ERROR)
			gflog_fatal(GFARM_MSG_1002972,
			    "Failed to add self mdhost");
		else if (gfarm_get_metadb_replication_enabled()) {
			gflog_info(GFARM_MSG_1002973,
			    "mdhost '%s' not found, creating...",
			    metadb_server_name);
			if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR)
				gflog_fatal(GFARM_MSG_1002974,
				    "Failed to add self mdhost");
		}
	}
	mdhost_self = self;
	if (gfarm_get_metadb_replication_enabled()) {
		FOREACH_MDHOST(it) {
			mh = mdhost_iterator_access(&it);
			if (!mdhost_is_valid(mh))
				continue;
			if (mdhost_is_default_master(mh)) {
				mdhost_set_is_master(mh, 1);
				break;
			}
		}
		if ((e = mdhost_updated()) != GFARM_ERR_NO_ERROR)
			gflog_fatal(GFARM_MSG_1002975,
			    "Failed to update mdhost: %s",
			    gfarm_error_string(e));
	}
	mdhost_activate(self);
}

struct thread_pool *
mdhost_send_manager_get_thrpool(void)
{
	return (netsendq_manager_get_thrpool(mdhost_send_manager));
}
