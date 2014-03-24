struct gfm_connection;
struct gfs_dirent;

struct gfarm_host_info;
struct gfarm_fsngroup_info;
struct gfarm_user_info;
struct gfarm_group_info;
struct gfarm_group_names;
struct gfarm_quota_get_info;
struct gfarm_quota_set_info;

struct gfarm_host_sched_info {
	char *host;
	gfarm_uint32_t port;
	gfarm_uint32_t ncpu;	/* XXX should have whole gfarm_host_info? */

	/* if GFM_PROTO_SCHED_FLAG_LOADAVG_AVAIL */
	gfarm_int32_t loadavg; /* loadavg_1min * GFM_PROTO_LOADAVG_FSCALE */
	gfarm_uint64_t cache_time;
	gfarm_uint64_t disk_used;
	gfarm_uint64_t disk_avail;

	/* if GFM_PROTO_SCHED_FLAG_RTT_AVAIL */
	gfarm_uint64_t rtt_cache_time;
	gfarm_uint32_t rtt_usec;

	gfarm_uint32_t flags;			/* GFM_PROTO_SCHED_FLAG_* */
};
void gfarm_host_sched_info_free(int, struct gfarm_host_sched_info *);

int gfm_client_is_connection_error(gfarm_error_t);
struct gfp_xdr *gfm_client_connection_conn(struct gfm_connection *);
int gfm_client_connection_fd(struct gfm_connection *);
enum gfarm_auth_method gfm_client_connection_auth_method(
	struct gfm_connection *);

int gfm_client_is_connection_valid(struct gfm_connection *);
const char *gfm_client_hostname(struct gfm_connection *);
const char *gfm_client_username(struct gfm_connection *);
int gfm_client_port(struct gfm_connection *);
gfarm_error_t gfm_client_source_port(struct gfm_connection *gfm_server, int *);
gfarm_error_t gfm_client_set_username_for_gsi(struct gfm_connection *,
	const char *);
struct gfarm_metadb_server *gfm_client_connection_get_real_server(
	struct gfm_connection *);
int gfm_client_connection_failover_count(struct gfm_connection *);

gfarm_error_t gfm_client_process_get(struct gfm_connection *,
	gfarm_int32_t *, const char **, size_t *, gfarm_pid_t *);
gfarm_error_t gfm_client_process_is_set(struct gfm_connection *);
int gfm_cached_connection_had_connection_error(struct gfm_connection *);

gfarm_error_t gfm_client_connection_acquire(const char *, int, const char *,
	struct gfm_connection **);
gfarm_error_t gfm_client_connection_addref(struct gfm_connection *);
gfarm_error_t gfm_client_connection_and_process_acquire(const char *, int,
	const char *, struct gfm_connection **);
gfarm_error_t gfm_client_connect(const char *, int, const char *,
	struct gfm_connection **, const char *);
struct passwd;
gfarm_error_t gfm_client_connect_with_seteuid(const char *, int, const char *,
	struct gfm_connection **, const char *, struct passwd *, int);
void gfm_client_connection_free(struct gfm_connection *);
struct gfp_xdr *gfm_client_connection_convert_to_xdr(struct gfm_connection *);
void gfm_client_terminate(void);

void gfm_client_connection_lock(struct gfm_connection *);
void gfm_client_connection_unlock(struct gfm_connection *);

/* host/user/group metadata */

gfarm_error_t gfm_client_host_info_get_all(struct gfm_connection *,
	int *, struct gfarm_host_info **);
gfarm_error_t gfm_client_host_info_get_by_architecture(
	struct gfm_connection *, const char *,
	int *, struct gfarm_host_info **);
gfarm_error_t gfm_client_host_info_get_by_names(struct gfm_connection *,
	int, const char **,
	gfarm_error_t *, struct gfarm_host_info *);
gfarm_error_t gfm_client_host_info_get_by_namealiases(struct gfm_connection *,
	int, const char **,
	gfarm_error_t *, struct gfarm_host_info *);
gfarm_error_t gfm_client_host_info_set(struct gfm_connection *,
	const struct gfarm_host_info *);
gfarm_error_t gfm_client_host_info_modify(struct gfm_connection *,
	const struct gfarm_host_info *);
gfarm_error_t gfm_client_host_info_remove(struct gfm_connection *,
	const char *);
gfarm_error_t gfm_client_fsngroup_get_all(struct gfm_connection *,
	size_t *, struct gfarm_fsngroup_info **);
gfarm_error_t gfm_client_fsngroup_get_by_hostname(struct gfm_connection *,
	const char *, char **);
gfarm_error_t gfm_client_fsngroup_modify(struct gfm_connection *,
	struct gfarm_fsngroup_info *);

gfarm_error_t gfm_client_user_info_get_all(struct gfm_connection *,
	int *, struct gfarm_user_info **);
gfarm_error_t gfm_client_user_info_get_by_names(struct gfm_connection *,
	int, const char **, gfarm_error_t *, struct gfarm_user_info *);
gfarm_error_t gfm_client_user_info_get_by_gsi_dn(struct gfm_connection *,
	const char *, struct gfarm_user_info *);
gfarm_error_t gfm_client_user_info_set(struct gfm_connection *,
	const struct gfarm_user_info *);
gfarm_error_t gfm_client_user_info_modify(struct gfm_connection *,
	const struct gfarm_user_info *);
gfarm_error_t gfm_client_user_info_remove(struct gfm_connection *,
	const char *);

gfarm_error_t gfm_client_group_info_get_all(struct gfm_connection *,
	int *, struct gfarm_group_info **);
gfarm_error_t gfm_client_group_info_get_by_names(struct gfm_connection *,
	int, const char **,
	gfarm_error_t *, struct gfarm_group_info *);
gfarm_error_t gfm_client_group_info_set(struct gfm_connection *,
	const struct gfarm_group_info *);
gfarm_error_t gfm_client_group_info_modify(struct gfm_connection *,
	const struct gfarm_group_info *);
gfarm_error_t gfm_client_group_info_remove(struct gfm_connection *,
	const char *);
gfarm_error_t gfm_client_group_info_add_users(struct gfm_connection *,
	const char *, int, const char **, gfarm_error_t *);
gfarm_error_t gfm_client_group_info_remove_users(
	struct gfm_connection *, const char *, int,
	const char **, gfarm_error_t *);
gfarm_error_t gfm_client_group_names_get_by_users(struct gfm_connection *,
	int, const char **,
	gfarm_error_t *, struct gfarm_group_names *);

struct gfp_xdr_context;
struct gfp_xdr_xid_record;
gfarm_error_t gfm_client_context_alloc(struct gfm_connection *,
	struct gfp_xdr_context **);
void gfm_client_context_free(struct gfm_connection *,
	struct gfp_xdr_context *);
void gfm_client_context_free_until(struct gfm_connection *,
	struct gfp_xdr_context *, struct gfp_xdr_xid_record *);
struct gfp_xdr_xid_record *gfm_client_context_get_pos(struct gfm_connection *,
	struct gfp_xdr_context *);

/* gfs from client */
gfarm_error_t gfm_client_compound_begin_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_begin_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_end_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_end_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_until_eof_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_until_eof_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_on_eof_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_on_eof_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_compound_on_error_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_error_t);
gfarm_error_t gfm_client_compound_on_error_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_get_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_get_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t *);
gfarm_error_t gfm_client_put_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_put_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_save_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_save_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_restore_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_restore_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_create_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	const char *, gfarm_uint32_t, gfarm_uint32_t);
gfarm_error_t gfm_client_create_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_ino_t *, gfarm_uint64_t *, gfarm_mode_t *);
gfarm_error_t gfm_client_open_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, size_t, gfarm_uint32_t);
gfarm_error_t gfm_client_open_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_ino_t *, gfarm_uint64_t *, gfarm_mode_t *);
gfarm_error_t gfm_client_open_root_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_uint32_t);
gfarm_error_t gfm_client_open_root_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_open_parent_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_uint32_t);
gfarm_error_t gfm_client_open_parent_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_close_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_close_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_close_read_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_close_read_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_close_write_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_off_t,
	gfarm_int64_t, gfarm_int32_t, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_close_write_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_close_write_v2_4_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_off_t,
	gfarm_int64_t, gfarm_int32_t, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_close_write_v2_4_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_int32_t *, gfarm_int64_t *, gfarm_int64_t *);
gfarm_error_t gfm_client_fhclose_read(struct gfm_connection *,
	gfarm_ino_t, gfarm_uint64_t, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_fhclose_write(struct gfm_connection *,
	gfarm_ino_t, gfarm_uint64_t, gfarm_off_t,
	gfarm_int64_t, gfarm_int32_t, gfarm_int64_t, gfarm_int32_t,
	gfarm_int32_t *, gfarm_int64_t *, gfarm_int64_t *, gfarm_uint64_t *);
gfarm_error_t gfm_client_generation_updated_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_generation_updated_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_generation_updated_by_cookie(struct gfm_connection *,
	gfarm_uint64_t, gfarm_int32_t);
gfarm_error_t gfm_client_verify_type_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_verify_type_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_verify_type_not_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_verify_type_not_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_revoke_gfsd_access(struct gfm_connection *,
	gfarm_int32_t);
gfarm_error_t gfm_client_bequeath_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_bequeath_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_inherit_fd_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_inherit_fd_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_fstat_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_fstat_result(struct gfm_connection *,
	struct gfp_xdr_context *, struct gfs_stat *);
gfarm_error_t gfm_client_fgetattrplus_request(struct gfm_connection *,
	struct gfp_xdr_context *, char **, int, int);
gfarm_error_t gfm_client_fgetattrplus_result(struct gfm_connection *,
	struct gfp_xdr_context *, struct gfs_stat *, int *,
	char ***, void ***, size_t **);
gfarm_error_t gfm_client_futimes_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_int64_t, gfarm_int32_t, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_futimes_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_fchmod_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_mode_t);
gfarm_error_t gfm_client_fchmod_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_fchown_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, const char *);
gfarm_error_t gfm_client_fchown_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_cksum_get_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_cksum_get_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	char **, size_t, size_t *, char *, gfarm_int32_t *);
gfarm_error_t gfm_client_cksum_set_request(struct gfm_connection *,
	struct gfp_xdr_context *, char *, size_t, const char *,
	gfarm_int32_t, gfarm_int64_t, gfarm_int32_t);
gfarm_error_t gfm_client_cksum_set_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_schedule_file_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_schedule_file_result(
	struct gfm_connection *, struct gfp_xdr_context *,
	int *, struct gfarm_host_sched_info **);
gfarm_error_t gfm_client_schedule_file_with_program_request(
	struct gfm_connection *, struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_schedule_file_with_program_result(
	struct gfm_connection *, struct gfp_xdr_context *,
	int *, struct gfarm_host_sched_info **);
gfarm_error_t gfm_client_schedule_host_domain(struct gfm_connection *,
	const char *, int *, struct gfarm_host_sched_info **);
gfarm_error_t gfm_client_remove_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_remove_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_rename_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, const char *);
gfarm_error_t gfm_client_rename_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_flink_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_flink_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_mkdir_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, gfarm_mode_t);
gfarm_error_t gfm_client_mkdir_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_symlink_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, const char *);
gfarm_error_t gfm_client_symlink_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_readlink_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_readlink_result(struct gfm_connection *,
	struct gfp_xdr_context *, char **);
gfarm_error_t gfm_client_getdirpath_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_getdirpath_result(struct gfm_connection *,
	struct gfp_xdr_context *, char **);
gfarm_error_t gfm_client_getdirents_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_getdirents_result(struct gfm_connection *,
	struct gfp_xdr_context *, int *, struct gfs_dirent *);
gfarm_error_t gfm_client_getdirentsplus_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_getdirentsplus_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	int *, struct gfs_dirent *, struct gfs_stat *);
gfarm_error_t gfm_client_getdirentsplusxattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t, char **, int);
gfarm_error_t gfm_client_getdirentsplusxattr_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	int *, struct gfs_dirent *, struct gfs_stat *,
	int *, char ***, void ***, size_t **);
gfarm_error_t gfm_client_seek_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_off_t, gfarm_int32_t);
gfarm_error_t gfm_client_seek_result(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_off_t *);
gfarm_error_t gfm_client_statfs(struct gfm_connection *,
	gfarm_off_t *, gfarm_off_t *, gfarm_off_t *);

gfarm_error_t gfm_client_setxattr_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	int, const char *, const void *, size_t, int);
gfarm_error_t gfm_client_setxmlattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, const void *, size_t, int);
gfarm_error_t gfm_client_setxattr_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_setxmlattr_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_getxattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, int, const char *);
gfarm_error_t gfm_client_getxmlattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_getxattr_result(struct gfm_connection *,
	struct gfp_xdr_context *, int, void **, size_t *);
gfarm_error_t gfm_client_getxmlattr_result(struct gfm_connection *,
	struct gfp_xdr_context *, void **, size_t *);
gfarm_error_t gfm_client_listxattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, int);
gfarm_error_t gfm_client_listxmlattr_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_listxattr_result(struct gfm_connection *,
	struct gfp_xdr_context *, char **, size_t *);
gfarm_error_t gfm_client_listxmlattr_result(struct gfm_connection *,
	struct gfp_xdr_context *, char **, size_t *);
gfarm_error_t gfm_client_removexattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, int, const char *);
gfarm_error_t gfm_client_removexmlattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_removexattr_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_removexmlattr_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_findxmlattr_request(struct gfm_connection *,
	struct gfp_xdr_context *, struct gfs_xmlattr_ctx *ctxp);
gfarm_error_t gfm_client_findxmlattr_result(struct gfm_connection *,
	struct gfp_xdr_context *, struct gfs_xmlattr_ctx *ctxp);

gfarm_error_t gfm_client_quota_user_get(struct gfm_connection *, const char *,
					struct gfarm_quota_get_info *);
gfarm_error_t gfm_client_quota_user_set(struct gfm_connection *,
					struct gfarm_quota_set_info *);
gfarm_error_t gfm_client_quota_group_get(struct gfm_connection *, const char *,
					 struct gfarm_quota_get_info *);
gfarm_error_t gfm_client_quota_group_set(struct gfm_connection *,
					struct gfarm_quota_set_info *);
gfarm_error_t gfm_client_quota_check(struct gfm_connection *);

/* gfs from gfsd */
gfarm_error_t gfm_client_reopen_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_reopen_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_ino_t *, gfarm_uint64_t *, gfarm_int32_t *, gfarm_int32_t *,
	gfarm_int32_t *);
gfarm_error_t gfm_client_lock_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_off_t, gfarm_off_t, gfarm_int32_t, gfarm_int32_t);
gfarm_error_t gfm_client_lock_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_trylock_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_off_t, gfarm_off_t, gfarm_int32_t, gfarm_int32_t);
gfarm_error_t gfm_client_trylock_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_unlock_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_off_t, gfarm_off_t, gfarm_int32_t, gfarm_int32_t);
gfarm_error_t gfm_client_unlock_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_lock_info_request(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_off_t, gfarm_off_t, gfarm_int32_t, gfarm_int32_t);
gfarm_error_t gfm_client_lock_info_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_off_t *, gfarm_off_t *, gfarm_int32_t *, char **, gfarm_pid_t *);
#ifdef COMPAT_GFARM_2_3
gfarm_error_t gfm_client_switch_back_channel(struct gfm_connection *);
#endif
gfarm_error_t gfm_client_switch_async_back_channel(struct gfm_connection *,
	gfarm_int32_t, gfarm_int64_t, gfarm_int32_t *);
gfarm_error_t gfm_client_switch_gfmd_channel(struct gfm_connection *,
	gfarm_int32_t, gfarm_int64_t, gfarm_int32_t *);

/* gfs_pio from client */
/*XXX*/

/* misc operations from gfsd */
gfarm_error_t gfm_client_hostname_set(struct gfm_connection *, const char *);

/* replica management from client */
gfarm_error_t gfm_client_replica_list_by_name_request(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_replica_list_by_name_result(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t *, char ***);
gfarm_error_t gfm_client_replica_list_by_host_request(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, gfarm_int32_t);
gfarm_error_t gfm_client_replica_list_by_host_result(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t *, gfarm_ino_t **);
gfarm_error_t gfm_client_replica_remove_by_host_request(
	struct gfm_connection *, struct gfp_xdr_context *,
	const char *, gfarm_int32_t);
gfarm_error_t gfm_client_replica_remove_by_host_result(
	struct gfm_connection *, struct gfp_xdr_context *);
gfarm_error_t gfm_client_replica_remove_by_file_request(
	struct gfm_connection *, struct gfp_xdr_context *, const char *);
gfarm_error_t gfm_client_replica_remove_by_file_result(
	struct gfm_connection *, struct gfp_xdr_context *);
gfarm_error_t gfm_client_replica_info_get_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t);
gfarm_error_t gfm_client_replica_info_get_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_int32_t *, char ***, gfarm_uint64_t **, gfarm_int32_t **);
gfarm_error_t gfm_client_replicate_file_from_to_request(
	struct gfm_connection *, struct gfp_xdr_context *,
	const char *, const char *, gfarm_int32_t);
gfarm_error_t gfm_client_replicate_file_from_to_result(
	struct gfm_connection *, struct gfp_xdr_context *);
gfarm_error_t gfm_client_replicate_file_from_request(
	struct gfm_connection *, struct gfp_xdr_context *,
	const char *, const char *, gfarm_int32_t);
gfarm_error_t gfm_client_replicate_file_from_result(
	struct gfm_connection *, struct gfp_xdr_context *);

/* replica management from gfsd */
gfarm_error_t gfm_client_replica_adding_request(struct gfm_connection *,
	struct gfp_xdr_context *, char *);
gfarm_error_t gfm_client_replica_adding_result(struct gfm_connection *,
	struct gfp_xdr_context *,
	gfarm_ino_t *, gfarm_uint64_t *, gfarm_int64_t *, gfarm_int32_t *);
#ifdef COMPAT_GFARM_2_3
gfarm_error_t gfm_client_replica_added_request(struct gfm_connection *,
	struct gfp_xdr_context *, gfarm_int32_t, gfarm_int64_t, gfarm_int32_t);
#endif
gfarm_error_t gfm_client_replica_added2_request(
	struct gfm_connection *, struct gfp_xdr_context *,
	gfarm_int32_t, gfarm_int64_t, gfarm_int32_t, gfarm_off_t);
gfarm_error_t gfm_client_replica_added2_result(struct gfm_connection *,
	struct gfp_xdr_context *);
gfarm_error_t gfm_client_replica_lost(struct gfm_connection *,
	gfarm_ino_t, gfarm_uint64_t);
gfarm_error_t gfm_client_replica_add(struct gfm_connection *,
	gfarm_ino_t, gfarm_uint64_t, gfarm_off_t);
gfarm_error_t gfm_client_replica_get_my_entries(
	struct gfm_connection *, gfarm_ino_t, int,
	int *, gfarm_ino_t **, gfarm_uint64_t **, gfarm_off_t **);
gfarm_error_t gfm_client_replica_create_file_in_lost_found(
	struct gfm_connection *, gfarm_ino_t, gfarm_uint64_t, gfarm_off_t,
	const struct gfarm_timespec *,
	gfarm_ino_t *, gfarm_uint64_t *);

/* process management */
gfarm_error_t gfm_client_process_alloc(struct gfm_connection *,
	gfarm_int32_t, const char *, size_t, gfarm_pid_t *);
#ifdef NOT_USED
gfarm_error_t gfm_client_process_alloc_child(struct gfm_connection *,
	gfarm_int32_t, const char *, size_t, gfarm_pid_t,
	gfarm_int32_t, const char *, size_t, gfarm_pid_t *);
#endif
gfarm_error_t gfm_client_process_free(struct gfm_connection *);
gfarm_error_t gfm_client_process_set(struct gfm_connection *,
	const char *user, gfarm_int32_t, const char *, size_t, gfarm_pid_t);

/* compound request - convenience function */
gfarm_error_t gfm_client_compound_fd_op(struct gfm_connection *, gfarm_int32_t,
	gfarm_error_t (*)(struct gfm_connection *,
	    struct gfp_xdr_context *, void *),
	gfarm_error_t (*)(struct gfm_connection *,
	    struct gfp_xdr_context *, void *),
	void (*)(struct gfm_connection *, void *),
	void *);

/* metadb_server metadata */
gfarm_error_t gfm_client_metadb_server_get(struct gfm_connection *,
	const char *, struct gfarm_metadb_server *);
gfarm_error_t gfm_client_metadb_server_get_all(struct gfm_connection *, int *,
	struct gfarm_metadb_server **);
gfarm_error_t gfm_client_metadb_server_set(struct gfm_connection *,
	struct gfarm_metadb_server *);
gfarm_error_t gfm_client_metadb_server_modify(struct gfm_connection *,
	struct gfarm_metadb_server *);
gfarm_error_t gfm_client_metadb_server_remove(struct gfm_connection *,
	const char *);

/* exported for a use from a private extension */
gfarm_error_t gfm_client_rpc_request(struct gfm_connection *,
	struct gfp_xdr_context *, int, const char *, ...);
gfarm_error_t gfm_client_rpc_result(struct gfm_connection *,
	struct gfp_xdr_context *, const char *, ...);
gfarm_error_t gfm_client_rpc(struct gfm_connection *,
	int, const char *, ...);
gfarm_error_t gfm_client_get_schedule_result(struct gfm_connection *,
	struct gfp_xdr_context *, int *, struct gfarm_host_sched_info **);
void gfm_client_purge_from_cache(struct gfm_connection *);
gfarm_error_t gfm_client_get_nhosts(struct gfm_connection *,
	size_t *, int, struct gfarm_host_info *);
