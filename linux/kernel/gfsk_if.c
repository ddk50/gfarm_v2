#include <gfarm/gflog.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/in.h>
#include <linux/syscalls.h>
#include <linux/parser.h>
#include <gfarm/gfarm.h>
#include <gfarm/gfarm_config.h>
#include "context.h"
#include "config.h"
#include "gfsk_fs.h"

#define FILE2INODE(file)	((file)->f_path.dentry->d_inode)
/*
 *  set file descriptor of current task into the file table of currrent fs
 *  return index of file table, otherwise error.
 */
int
gfsk_fd_set(int usrfd, int type)
{
	int	err = -EBADF;
	struct file *file;

	if (!(file = fget(usrfd))) {
		gflog_error(GFARM_MSG_UNFIXED, "invalid fd=%d\n", usrfd);
		goto out;
	}
	if (type && ((FILE2INODE(file)->i_mode) & S_IFMT) != type) {
		gflog_error(GFARM_MSG_UNFIXED, "invalid type=%x:%x\n",
			FILE2INODE(file)->i_mode, type);
		goto out_fput;
	}
	if ((err = gfsk_fd_file_set(file)) < 0) {
		gflog_error(GFARM_MSG_UNFIXED,
			"fail gfsk_fd_file_set %d\n", err);
		goto out_fput;
	}
out_fput:
	fput(file);
out:
	return (err);
}
int
gfsk_localfd_set(int usrfd, int type)
{
	int	err = -EBADF;
	struct file *file;

	if (!(file = fget(usrfd))) {
		gflog_error(GFARM_MSG_UNFIXED, "invalid fd=%d\n", usrfd);
		goto out;
	}
	if (type && ((FILE2INODE(file)->i_mode) & S_IFMT) != type) {
		gflog_error(GFARM_MSG_UNFIXED, "invalid type=%x:%x\n",
			FILE2INODE(file)->i_mode, type);
		goto out_fput;
	}
	if ((err = gfsk_fd_file_set(file)) < 0) {
		gflog_error(GFARM_MSG_UNFIXED,
			"fail gfsk_fd_file_set %d\n", err);
		goto out_fput;
	}
out_fput:
	fput(file);		/* ref : fget */
	sys_close(usrfd);	/* always close user fd */
out:
	return (err);
}
/*
 *  request helper daemon to connect server as user
 *  return index of file table of this fs.
 */
int
gfsk_gfmd_connect(const char *hostname, int port, const char *source_ip,
		const char *user, int *sock)
{
	int	err = -EINVAL;
	struct gfsk_req_connect inarg;
	struct gfsk_rpl_connect outarg;

	*sock = -1;
	if (strlen(hostname) >= sizeof(inarg.r_hostname)) {
		gflog_error(GFARM_MSG_UNFIXED,
			"hostname(%s) too long\n", hostname);
		goto out;
	}
	if (gfsk_fsp->gf_mdata.m_mfd >= 0) {
		if (!strcmp(hostname, gfsk_fsp->gf_mdata.m_host)
			&& !strcmp(user, gfsk_fsp->gf_mdata.m_uidname)) {
			*sock = gfsk_fsp->gf_mdata.m_mfd;
			gfsk_fsp->gf_mdata.m_mfd = -1;
			err = 0;
			goto out;
		}
		gflog_debug(GFARM_MSG_UNFIXED,
			"(host,user)=(%s,%s) != mount.arg(%s,%s)",
			hostname, user, gfsk_fsp->gf_mdata.m_host,
			gfsk_fsp->gf_mdata.m_uidname);
	}
	if (strlen(user) >= sizeof(inarg.r_global)) {
		gflog_error(GFARM_MSG_UNFIXED, "user(%s) too long\n", user);
		goto out;
	}
	if (ug_map_name_to_uid(user, strlen(user), &inarg.r_uid)) {
		gflog_error(GFARM_MSG_UNFIXED, "invalid user(%s)\n", user);
		goto out;
	}
	if (source_ip && strlen(source_ip) >= sizeof(inarg.r_source_ip)) {
		gflog_error(GFARM_MSG_UNFIXED, "source_ip(%s) too long\n",
						source_ip);
		goto out;
	}
	strcpy(inarg.r_hostname, hostname);
	inarg.r_port = port;
	if (source_ip)
		strcpy(inarg.r_source_ip, source_ip);
	strcpy(inarg.r_global, user);
	if ((err = gfsk_req_connect_sync(GFSK_OP_CONNECT_GFMD,
		inarg.r_uid, &inarg, &outarg))) {
		gflog_error(GFARM_MSG_UNFIXED, "connect fail err=%d\n", err);
		goto out;
	}
	/* fd is converted into fsfd from taskfd */
	*sock = outarg.r_fd;
out:
	return (err);
}
/*
 *  request helper daemon to connect server as user
 *  return index of file table of this fs.
 */
int
gfsk_gfsd_connect(const char *hostname,  struct sockaddr *peer_addr,
		const char *source_ip, const char *user,
		int *sock, void **kevpp, int evfd)
{
	int	err = -EINVAL;
	struct gfsk_req_connect *inarg;
	struct gfsk_rpl_connect *outarg;
	struct sockaddr_in *inp = (struct sockaddr_in *)peer_addr;

	if (!(inarg = kmalloc(sizeof(*inarg) + sizeof(*outarg), GFP_KERNEL))) {
		gflog_error(GFARM_MSG_UNFIXED,
			"can't alloc args %ld", sizeof(*inarg)
						+ sizeof(*outarg));
		err = -ENOMEM;
		goto out;
	}
	memset(inarg, 0, sizeof(*inarg) + sizeof(*outarg));
	outarg = (struct gfsk_rpl_connect *)(inarg + 1);
	if (strlen(hostname) >= sizeof(inarg->r_hostname)) {
		gflog_error(GFARM_MSG_UNFIXED, "hostname(%s) too long\n",
			hostname);
		goto out;
	}
	if (strlen(user) >= sizeof(inarg->r_global)) {
		gflog_error(GFARM_MSG_UNFIXED, "user(%s) too long\n", user);
		goto out;
	}
	if (source_ip && strlen(source_ip) >= sizeof(inarg->r_source_ip)) {
		gflog_error(GFARM_MSG_UNFIXED, "source_ip(%s) too long\n",
			source_ip);
		goto out;
	}
	if (peer_addr->sa_family != AF_INET) {
		gflog_error(GFARM_MSG_UNFIXED, "peer_addr is not AF_INET, %d\n",
			peer_addr->sa_family);
		goto out;
	}
	strcpy(inarg->r_hostname, hostname);
	inarg->r_port = ntohs(inp->sin_port);
	inarg->r_v4addr = ntohl(inp->sin_addr.s_addr);
	if (source_ip)
		strcpy(inarg->r_source_ip, source_ip);
	strcpy(inarg->r_global, user);
	inarg->r_uid = current_fsuid();
	if (kevpp) {
		if ((err = gfsk_req_connect_async(GFSK_OP_CONNECT_GFSD,
			inarg->r_uid, inarg, outarg, kevpp, evfd))) {
			gflog_error(GFARM_MSG_UNFIXED,
				"connect_async fail err=%d\n", err);
			goto out;
		}
	} else {
		if ((err = gfsk_req_connect_sync(GFSK_OP_CONNECT_GFSD,
			inarg->r_uid, inarg, outarg))) {
			gflog_error(GFARM_MSG_UNFIXED,
				"connect_sync fail err=%d\n", err);
			goto out;
		} else
			*sock = outarg->r_fd;
	}
out:
	if ((err || !kevpp) && inarg)
		kfree(inarg);
	return (err);
}
enum {
	OPT_RW,
	OPT_ON_DEMAND_REPLICATION,
	OPT_CALL_FTRUNCATE,
	OPT_BLKSIZE,
	OPT_ERR
};
static const match_table_t tokens = {
	{OPT_RW,		"rw"},
	{OPT_CALL_FTRUNCATE,		"call_ftruncate"},
	{OPT_ON_DEMAND_REPLICATION,	"on_demand_replication"},
	{OPT_BLKSIZE,			"blksize=%u"},
	{OPT_ERR,			NULL}
};
static int
gfsk_mount_options(struct gfsk_mount_data *mdatap)
{
	char *p, *opt = mdatap->m_opt;

	gfsk_fsp->gf_actime = msecs_to_jiffies(gfarm_ctxp->attr_cache_timeout);
	gfsk_fsp->gf_pctime = msecs_to_jiffies(gfarm_ctxp->page_cache_timeout);

	gfarm_ctxp->call_rpc_instead_syscall = 1;

	while ((p = strsep(&opt, ",")) != NULL) {
		int token;
		int value;
		substring_t args[MAX_OPT_ARGS];
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case OPT_RW:
			/* "rw" is always set by mount(3) */
				break;
		case OPT_ON_DEMAND_REPLICATION:
			gfarm_ctxp->on_demand_replication = 1;
			break;
		case OPT_CALL_FTRUNCATE:
			gfarm_ctxp->call_rpc_instead_syscall = 0;
			break;
		case OPT_BLKSIZE:
			if (match_int(&args[0], &value))
				goto error;
			break;
		default:
error:
			gflog_error(GFARM_MSG_UNFIXED, "unknown option %s", p);
			return (-EINVAL);
		}
	}
	return (0);
}

int
gfsk_client_mount(struct super_block *sb, void *arg)
{
	struct gfsk_mount_data	*mdatap = (struct gfsk_mount_data *) arg;
	struct gfsk_fs_context	*fsp;
	int	i, err;

	if (mdatap->m_version != GFSK_VER) {
		gflog_error(GFARM_MSG_UNFIXED,
			"version is expected %x, but %x", GFSK_VER,
				mdatap->m_version);
		err = -EINVAL;
		goto out;
	}
	if (!(fsp = kmalloc(sizeof(*fsp), GFP_KERNEL))) {
		gflog_error(GFARM_MSG_UNFIXED,
			"can't alloc gfsk_fs_context %ld", sizeof(*fsp));
		err = -ENOMEM;
		goto out;
	}
	memset(fsp, 0, sizeof(*fsp));
	fsp->gf_sb = sb;
	mutex_init(&fsp->gf_lock);
	INIT_LIST_HEAD(&fsp->gf_locallist);
	sb->s_fs_info = fsp;
	gfsk_fsp = fsp;

	gfsk_fsp->gf_mdata.m_version = mdatap->m_version;
	gfsk_fsp->gf_mdata.m_mfd = gfsk_fsp->gf_mdata.m_dfd = -1;

	gfsk_fsp->gf_mdata.m_uid = mdatap->m_uid;
	ug_map_uid_to_name(mdatap->m_uid, gfsk_fsp->gf_mdata.m_uidname,
		sizeof(gfsk_fsp->gf_mdata.m_uidname));

	if ((err = gfsk_fdset_init())) {
		goto out;
	}

	err = -ENOMEM;
	for (i = 0; i < GFSK_FBUF_MAX; i++) {
		struct gfsk_fbuf *fbp = &mdatap->m_fbuf[i];
		struct gfsk_fbuf *tbp = &gfsk_fsp->gf_mdata.m_fbuf[i];
		if (!fbp->f_name.d_len)
			continue;
		if (!(tbp->f_name.d_buf = memdup_user(fbp->f_name.d_buf,
			fbp->f_name.d_len))) {
			gflog_error(GFARM_MSG_UNFIXED,
				"Can't allocate for %i name, size %d",
					i, fbp->f_name.d_len);
			goto out;
		}
		tbp->f_name.d_len = fbp->f_name.d_len;
		if (!(tbp->f_buf.d_buf = memdup_user(fbp->f_buf.d_buf,
			fbp->f_buf.d_len))) {
			gflog_error(GFARM_MSG_UNFIXED,
				"Can't allocate for %i name, size %d",
					i, fbp->f_buf.d_len);
			goto out;
		}
		tbp->f_buf.d_len = fbp->f_buf.d_len;
	}

	err = -EINVAL;
	if (mdatap->m_mfd < 0
		|| (err = gfsk_fd_set(mdatap->m_mfd, S_IFSOCK)) < 0) {
		gflog_error(GFARM_MSG_UNFIXED,
			"invalid m_mfd '%d'", mdatap->m_mfd);
		goto out;
	}
	gfsk_fsp->gf_mdata.m_mfd = err;
	memcpy(gfsk_fsp->gf_mdata.m_host, mdatap->m_host,
				sizeof(mdatap->m_host));
	err = -EINVAL;
	if (mdatap->m_dfd < 0
		|| (err = gfsk_fd_set(mdatap->m_dfd, S_IFCHR)) < 0) {
		gflog_error(GFARM_MSG_UNFIXED,
			"invalid m_dfd '%d'", mdatap->m_dfd);
		goto out;
	}
	gfsk_fsp->gf_mdata.m_dfd = err;

	if ((err = gfsk_conn_init(err))) {
		gflog_error(GFARM_MSG_UNFIXED,
			"conn_init fail '%d'", mdatap->m_dfd);
		goto out;
	}
	if ((err = gfsk_gfarm_init(mdatap->m_uid))) {
		goto out;
	}
	/* Now, gfarm_ctxp has been initialized */

	if (!fsp->gf_gfarm_ctxp && gfsk_task_ctxp &&
			gfsk_task_ctxp->gk_gfarm_ctxp)
		fsp->gf_gfarm_ctxp = gfsk_task_ctxp->gk_gfarm_ctxp;

	if ((err = gfsk_mount_options(mdatap))) {
		goto out;
	}
out:
	return (err);
}

void
gfsk_client_unmount(void)
{
	struct gfsk_mount_data *mdatap;
	int	i;

	if (!gfsk_task_ctxp || !gfsk_fsp)
		return;

	mdatap = &gfsk_fsp->gf_mdata;

	if (mdatap->m_version <= 0)
		return;
	mdatap->m_version = -1;

	gfsk_local_map_fini(gfsk_fsp);

	gfsk_gfarm_fini();
	gfsk_fsp->gf_gfarm_ctxp = NULL;


	if (mdatap->m_dfd >= 0) {
		gfsk_conn_umount(mdatap->m_dfd);
		gfsk_fd_unset(mdatap->m_dfd);
		mdatap->m_dfd = -1;
	}
	if (mdatap->m_mfd >= 0) {
		gfsk_fd_unset(mdatap->m_mfd);
		mdatap->m_mfd = -1;
	}
	for (i = 0; i < GFSK_FBUF_MAX; i++) {
		struct gfsk_fbuf *tbp = &gfsk_fsp->gf_mdata.m_fbuf[i];
		if (!tbp->f_name.d_len)
			continue;
		kfree(tbp->f_name.d_buf);
		tbp->f_name.d_len = 0;
		if (!tbp->f_buf.d_len)
			continue;
		kfree(tbp->f_buf.d_buf);
		tbp->f_buf.d_len = 0;
	}
	gfsk_fdset_umount();
}

void
gfsk_client_fini(void)
{
	struct gfsk_mount_data *mdatap;

	if (!gfsk_task_ctxp || !gfsk_fsp)
		return;

	mdatap = &gfsk_fsp->gf_mdata;
	if (!mdatap->m_version)
		return;
	if (mdatap->m_version > 0) {
		gfsk_client_unmount();
	}
	mdatap->m_version = 0;

	gfsk_conn_fini();
	gfsk_fdset_fini();
}
