/*
 * $Id: host.h 8274 2013-06-17 07:21:40Z m-kasahr $
 */

void host_init(void);

struct abstract_host;
struct host;
struct sockaddr;
struct peer;
struct callout;
struct dead_file_copy;
struct netsendq;

struct host_status {
	double loadavg_1min, loadavg_5min, loadavg_15min;
	gfarm_off_t disk_used, disk_avail;
};

struct abstract_host *host_to_abstract_host(struct host *);

struct host *host_lookup(const char *);
struct host *host_lookup_including_invalid(const char *hostname);
struct host *host_lookup_at_loading(const char *);
struct host *host_addr_lookup(const char *, struct sockaddr *);

int host_status_callout_retry(struct host *);
void host_disconnect_request(struct host *, struct peer *);
struct callout *host_status_callout(struct host *);
struct peer *host_get_peer(struct host *);
struct peer *host_get_peer_by_generation(struct host *, gfarm_uint32_t);
void host_put_peer(struct host *, struct peer *);
gfarm_uint32_t host_get_peer_generation(struct host *h);

char *host_name(struct host *);
int host_port(struct host *);
char *host_architecture(struct host *);
int host_ncpu(struct host *);
int host_flags(struct host *);
char *host_fsngroup(struct host *);
struct netsendq *host_sendq(struct host *);
int host_supports_async_protocols(struct host *);
int host_is_disk_available(struct host *, gfarm_off_t);

#ifdef COMPAT_GFARM_2_3
void host_set_callback(struct abstract_host *, struct peer *,
	gfarm_int32_t (*)(void *, void *, size_t),
	void (*)(void *, void *), void *);
int host_get_result_callback(struct host *, struct peer *,
	gfarm_int32_t (**)(void *, void *, size_t), void **);
int host_get_disconnect_callback(struct host *,
	void (**)(void *, void *), struct peer **, void **);
#endif

int host_is_up(struct host *);
int host_is_up_with_grace(struct host *, gfarm_time_t);
int host_is_valid(struct host *);

int host_unique_sort(int, struct host **);
void host_intersect(int *, struct host **, int *, struct host **);
gfarm_error_t host_except(int *, struct host **, int *, struct host **,
	int (*)(struct host *, void *), void *);

gfarm_error_t host_is_disk_available_filter(struct host *, void *);
gfarm_error_t host_array_alloc(int *, struct host ***);
gfarm_error_t host_from_all(int (*)(struct host *, void *), void *,
	gfarm_int32_t *, struct host ***);
int host_number();

gfarm_error_t host_from_all_except(int *, struct host **,
	int (*)(struct host *, void *),	void *,
	gfarm_int32_t *, struct host ***);
gfarm_error_t host_schedule_n_except(
	int *, struct host **,
	int *, struct host **, gfarm_time_t,
	int *, struct host **,
	int (*)(struct host *, void *), void *,
	int, int *, struct host ***, int *);
void host_status_update(struct host *, struct host_status *);

struct gfarm_host_info;
gfarm_error_t host_enter(struct gfarm_host_info *, struct host **);
void host_modify(struct host *, struct gfarm_host_info *);
gfarm_error_t host_fsngroup_modify(struct host *, const char *, const char *);
gfarm_error_t host_remove_in_cache(const char *);

/* A generic function to select filesystem nodes */
gfarm_error_t host_iterate(int (*)(struct host *, void *, void *), void *,
	size_t, size_t *, void **);

gfarm_error_t gfm_server_host_info_get_all(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_get_by_architecture(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_get_by_names(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_get_by_namealiases(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_set(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_modify(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_host_info_remove(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);

gfarm_error_t host_schedule_reply(struct host *, struct peer *, const char *);
gfarm_error_t host_schedule_reply_all(struct peer *, gfp_xdr_xid_t, size_t *,
	int (*)(struct host *, void *), void *, const char *);
gfarm_error_t host_schedule_reply_arg_dynarg(struct host *, struct peer *,
	size_t *, const char *);

gfarm_error_t gfm_server_hostname_set(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_schedule_host_domain(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_statfs(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);


/* exported for a use from a private extension */
struct gfp_xdr;
gfarm_error_t host_info_send(struct gfp_xdr *, struct host *);
gfarm_error_t host_info_remove_default(const char *, const char *);
extern gfarm_error_t (*host_info_remove)(const char *, const char *);
