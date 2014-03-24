/*
 * $Id: quota.h 8235 2013-06-12 11:42:55Z takuya-i $
 */

/* related to struct user and struct group */
struct quota {
	int on_db;  /* 0: not exist on database and disable quota */
	gfarm_time_t    grace_period;
	gfarm_off_t     space; /* -1: disable, only memory */
	gfarm_time_t    space_exceed;
	gfarm_off_t     space_soft;
	gfarm_off_t     space_hard;
	gfarm_uint64_t  num; /* only memory */
	gfarm_time_t    num_exceed;
	gfarm_uint64_t  num_soft;
	gfarm_uint64_t  num_hard;
	gfarm_off_t     phy_space; /* only memory */
	gfarm_time_t    phy_space_exceed;
	gfarm_off_t     phy_space_soft;
	gfarm_off_t     phy_space_hard;
	gfarm_uint64_t  phy_num; /* only memory */
	gfarm_time_t    phy_num_exceed;
	gfarm_uint64_t  phy_num_soft;
	gfarm_uint64_t  phy_num_hard;
};

void quota_init(void);

void quota_data_init(struct quota *);

struct user;
struct group;
gfarm_error_t quota_check_limits(struct user *, struct group *, int, int);
void quota_user_remove(struct user *);
void quota_group_remove(struct group *);

struct inode;
void quota_update_file_add(struct inode *);
void quota_update_file_resize(struct inode *, gfarm_off_t);
void quota_update_replica_add(struct inode *);
void quota_update_replica_remove(struct inode *);
void quota_update_file_remove(struct inode *);
gfarm_error_t quota_lookup(const char *, int, struct quota **, const char *);
void quota_check(void);

struct peer;
gfarm_error_t gfm_server_quota_user_get(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_quota_user_set(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_quota_group_get(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_quota_group_set(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
gfarm_error_t gfm_server_quota_check(
	struct peer *, gfp_xdr_xid_t, size_t *, int, int);
