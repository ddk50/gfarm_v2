#include <assert.h>
#include <stdio.h>

#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>	/* more portable than <stdint.h> on UNIX variants */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <netinet/in.h> /* ntoh[ls]()/hton[ls]() on glibc */
#include <gfarm/gfarm_config.h>
#include <gfarm/gflog.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <sqlite3.h>

#include "gfutil.h" /* gflog_fatal() */

#include "liberror.h"
#include "iobuffer.h"
#include "gfp_xdr.h"

#ifndef va_copy /* since C99 standard */
#define va_copy(dst, src)	((dst) = (src))
#endif

#ifndef INT64T_IS_FLOAT
#define INT64T_IS_FLOAT 0
#endif /* INT64T_IS_FLOAT */

#if INT64T_IS_FLOAT
#include <math.h>

#define POWER2_32	4294967296.0		/* 2^32 */
#endif

#define GFP_XDR_BUFSIZE	16384

struct gfp_xdr {
	struct gfarm_iobuffer *recvbuffer;
	struct gfarm_iobuffer *sendbuffer;

	struct gfp_iobuffer_ops *iob_ops;
	void *cookie;
	int fd;

	/* XXX currently used by client only, but should be used by servers */
	struct gfp_xdr_async_server *async;

	gfarm_uint32_t client_addr;

	gfarm_uint32_t total_cache_hit;
	gfarm_uint32_t total_read;
	double latest_cache_hitrate;
};

/*
 * switch to new iobuffer operation,
 * and (possibly) switch to new cookie/fd
 */
void
gfp_xdr_set(struct gfp_xdr *conn,
	struct gfp_iobuffer_ops *ops, void *cookie, int fd)
{
	conn->iob_ops = ops;
	conn->cookie = cookie;
	conn->fd = fd;

	if (conn->recvbuffer) {
		gfarm_iobuffer_set_read_timeout(conn->recvbuffer,
		    ops->blocking_read_timeout, cookie, fd);
		gfarm_iobuffer_set_read_notimeout(conn->recvbuffer,
		    ops->blocking_read_notimeout, cookie, fd);
	}
	if (conn->sendbuffer)
		gfarm_iobuffer_set_write(conn->sendbuffer, ops->blocking_write,
		    cookie, fd);
}

gfarm_error_t
gfp_xdr_new(struct gfp_iobuffer_ops *ops, void *cookie, int fd,
	int flags, struct gfp_xdr **connp)
{
	struct gfp_xdr *conn;

	GFARM_MALLOC(conn);
	if (conn == NULL) {
		gflog_debug(GFARM_MSG_1000996,
			"allocation of 'conn' failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	if ((flags & GFP_XDR_NEW_RECV) != 0) {
		conn->recvbuffer = gfarm_iobuffer_alloc(GFP_XDR_BUFSIZE);
		if (conn->recvbuffer == NULL) {
			free(conn);
			gflog_debug(GFARM_MSG_1000997,
				"allocation of 'conn recvbuffer' failed: %s",
				gfarm_error_string(GFARM_ERR_NO_MEMORY));
			return (GFARM_ERR_NO_MEMORY);
		}
		if ((flags & GFP_XDR_NEW_AUTO_RECV_EXPANSION) != 0) {
			gfarm_iobuffer_set_read_auto_expansion(
			    conn->recvbuffer, 1);
		}
	} else
		conn->recvbuffer = NULL;

	if ((flags & GFP_XDR_NEW_SEND) != 0) {
		conn->sendbuffer = gfarm_iobuffer_alloc(GFP_XDR_BUFSIZE);
		if (conn->sendbuffer == NULL) {
			gfarm_iobuffer_free(conn->recvbuffer);
			free(conn);
			gflog_debug(GFARM_MSG_1000998,
				"allocation of 'conn sendbuffer' failed: %s",
				gfarm_error_string(GFARM_ERR_NO_MEMORY));
			return (GFARM_ERR_NO_MEMORY);
		}
	} else
		conn->sendbuffer = NULL;

	gfp_xdr_set(conn, ops, cookie, fd);
	conn->async = NULL;

	*connp = conn;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_free(struct gfp_xdr *conn)
{
	gfarm_error_t e, e_save;

	e_save = gfp_xdr_flush(conn);
	gfarm_iobuffer_free(conn->sendbuffer);
	gfarm_iobuffer_free(conn->recvbuffer);

	e = (*conn->iob_ops->close)(conn->cookie, conn->fd);
	if (e_save == GFARM_ERR_NO_ERROR)
		e_save = e;

	free(conn);
	return (e);
}

void *
gfp_xdr_cookie(struct gfp_xdr *conn)
{
	return (conn->cookie);
}

int
gfp_xdr_fd(struct gfp_xdr *conn)
{
	return (conn->fd);
}

struct gfp_xdr_async_server *
gfp_xdr_async(struct gfp_xdr *conn)
{
	return (conn->async);
}

void
gfp_xdr_set_async(struct gfp_xdr *conn, struct gfp_xdr_async_server *async)
{
	conn->async = async;
}

gfarm_error_t
gfp_xdr_sendbuffer_check_size(struct gfp_xdr *conn, int size)
{
	if (size > gfarm_iobuffer_get_size(conn->sendbuffer))
		return (GFARM_ERR_NO_BUFFER_SPACE_AVAILABLE);
	else if (size > gfarm_iobuffer_avail_length(conn->sendbuffer))
		return (GFARM_ERR_RESOURCE_TEMPORARILY_UNAVAILABLE);
	else
		return (GFARM_ERR_NO_ERROR);
}

void
gfp_xdr_recvbuffer_clear_read_eof(struct gfp_xdr *conn)
{
	gfarm_iobuffer_clear_read_eof(conn->recvbuffer);
}

gfarm_error_t
gfp_xdr_export_credential(struct gfp_xdr *conn)
{
	return ((*conn->iob_ops->export_credential)(conn->cookie));
}

gfarm_error_t
gfp_xdr_delete_credential(struct gfp_xdr *conn, int sighandler)
{
	return ((*conn->iob_ops->delete_credential)(conn->cookie, sighandler));
}

char *
gfp_xdr_env_for_credential(struct gfp_xdr *conn)
{
	return ((*conn->iob_ops->env_for_credential)(conn->cookie));
}


int
gfp_xdr_recv_is_ready(struct gfp_xdr *conn)
{
	return (!gfarm_iobuffer_empty(conn->recvbuffer) ||
		gfarm_iobuffer_is_eof(conn->recvbuffer));
}

gfarm_error_t
gfp_xdr_flush(struct gfp_xdr *conn)
{
	if (conn->sendbuffer == NULL)
		return (GFARM_ERR_NO_ERROR);
	gfarm_iobuffer_flush_write(conn->sendbuffer);
	return (gfarm_iobuffer_get_error(conn->sendbuffer));
}

gfarm_error_t
gfp_xdr_purge_sized(struct gfp_xdr *conn, int just, int len, size_t *sizep)
{
	gfarm_error_t e;
	int rv;

	if (*sizep < len) {
		gflog_debug(GFARM_MSG_1000999, "gfp_xdr_purge_sized: "
		    "%d bytes expected, but only %d bytes remains",
		    len, (int)*sizep);
		return (GFARM_ERR_PROTOCOL);
	}
	/*
	 * always do timeout here,
	 * because a header/command/error must be already received
	 */
	rv = gfarm_iobuffer_purge_read_x(conn->recvbuffer, len, just, 1);
	*sizep -= rv;
	if (rv != len) {
		e = gfarm_iobuffer_get_error(conn->recvbuffer);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		return (GFARM_ERR_UNEXPECTED_EOF);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_purge(struct gfp_xdr *conn, int just, int len)
{
	/*
	 * always do timeout here,
	 * because a header/command/error must be already received
	 */
	if (gfarm_iobuffer_purge_read_x(conn->recvbuffer, len, just, 1)
	    != len) {
		gflog_debug(GFARM_MSG_1001000,
			"gfarm_iobuffer_purge_read_x() failed: %s",
			gfarm_error_string(GFARM_ERR_UNEXPECTED_EOF));
		return (GFARM_ERR_UNEXPECTED_EOF);
	}
	return (GFARM_ERR_NO_ERROR);
}

void
gfp_xdr_purge_all(struct gfp_xdr *conn)
{
	if (conn->sendbuffer)
		while (gfarm_iobuffer_purge(conn->sendbuffer, NULL) > 0)
			;
	if (conn->recvbuffer)
		while (gfarm_iobuffer_purge(conn->recvbuffer, NULL) > 0)
			;
}

gfarm_error_t
gfp_xdr_vsend_size_add(size_t *sizep, const char **formatp, va_list *app)
{
	const char *format = *formatp;
	size_t size = *sizep;
	gfarm_uint8_t c;
	gfarm_int16_t h;
	gfarm_int32_t i, n;
	gfarm_uint32_t lv[2];
#if INT64T_IS_FLOAT
	int minus;
#endif
#ifndef __KERNEL__	/* double */
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nd;
#else
	double d;
#	define nd d
#endif
#endif /* __KERNEL__ */
	const char *s;

	for (; *format; format++) {
		switch (*format) {
		case 'c':
			c = va_arg(*app, int);
			size += sizeof(c);
			continue;
		case 'h':
			h = va_arg(*app, int);
			size += sizeof(h);
			continue;
		case 'i':
			i = va_arg(*app, gfarm_int32_t);
			size += sizeof(i);
			continue;
		case 'l':
			/*
			 * note that because actual type of gfarm_int64_t
			 * may be diffenent (int64_t or double), we use lv here
			 */
			(void)va_arg(*app, gfarm_int64_t);
			size += sizeof(lv);
			continue;
		case 's':
			s = va_arg(*app, const char *);
			n = strlen(s);
			size += sizeof(i);
			size += n;
			continue;
		case 'S':
			(void)va_arg(*app, const char *);
			n = va_arg(*app, size_t);
			size += sizeof(i);
			size += n;
			continue;
		case 'b':
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			n = va_arg(*app, size_t);
			(void)va_arg(*app, const char *);
			size += sizeof(i);
			size += n;
			continue;
		case 'r':
			n = va_arg(*app, size_t);
			(void)va_arg(*app, const char *);
			size += n;
			continue;
		case 'f':
#ifndef __KERNEL__	/* double */
			(void)va_arg(*app, double);
			size += sizeof(nd);
			continue;
#else /* __KERNEL__ */
			gflog_fatal(GFARM_MSG_1003686, "floating format is not "
				"supported. '%s'", *formatp);
			return (GFARM_ERR_PROTOCOL);  /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			gflog_fatal(GFARM_MSG_1003687,
			    "gfp_xdr_vsend_size_add: unimplemented format '%c'",
			    *format);
			break;
		}

		break;
	}
	*sizep = size;
	*formatp = format;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_vsend(struct gfp_xdr *conn,
	const char **formatp, va_list *app)
{
	const char *format = *formatp;
	gfarm_uint8_t c;
	gfarm_int16_t h;
	gfarm_int32_t i, n;
	gfarm_int64_t o;
	gfarm_uint32_t lv[2];
#if INT64T_IS_FLOAT
	int minus;
#endif
#ifndef __KERNEL__	/* double */
	double d;
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nd;
#else
#	define nd d
#endif
#endif /* __KERNEL__ */
	const char *s;

	for (; *format; format++) {
		switch (*format) {
		case 'c':
			c = va_arg(*app, int);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &c, sizeof(c));
			continue;
		case 'h':
			h = va_arg(*app, int);
			h = htons(h);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &h, sizeof(h));
			continue;
		case 'i':
			i = va_arg(*app, gfarm_int32_t);
			i = htonl(i);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			continue;
		case 'l':
			/*
			 * note that because actual type of gfarm_int64_t
			 * may be diffenent (int64_t or double), we must
			 * not pass this as is via network.
			 */
			o = va_arg(*app, gfarm_int64_t);
#if INT64T_IS_FLOAT
			minus = o < 0;
			if (minus)
				o = -o;
			lv[0] = o / POWER2_32;
			lv[1] = o - lv[0] * POWER2_32;
			if (minus) {
				lv[0] = ~lv[0];
				lv[1] = ~lv[1];
				if (++lv[1] == 0)
					++lv[0];
			}
#else
			lv[0] = o >> 32;
			lv[1] = o;
#endif
			lv[0] = htonl(lv[0]);
			lv[1] = htonl(lv[1]);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    lv, sizeof(lv));
			continue;
		case 's':
			s = va_arg(*app, const char *);
			n = strlen(s);
			i = htonl(n);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'S':
			s = va_arg(*app, const char *);
			n = va_arg(*app, size_t);
			i = htonl(n);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'b':
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			n = va_arg(*app, size_t);
			i = htonl(n);
			s = va_arg(*app, const char *);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'r':
			n = va_arg(*app, size_t);
			s = va_arg(*app, const char *);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'f':
#ifndef __KERNEL__	/* double */

			d = va_arg(*app, double);
#ifndef WORDS_BIGENDIAN
			swab(&d, &nd, sizeof(nd));
#endif
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &nd, sizeof(nd));
			continue;
#else /* __KERNEL__ */
			gflog_fatal(GFARM_MSG_1003688, "floating format is not "
				"supported. '%s'", *formatp);
			return (GFARM_ERR_PROTOCOL);  /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			gflog_fatal(GFARM_MSG_1003689,
			    "gfp_xdr_vsend: unimplemented format '%c'",
			    *format);
			break;
		}

		break;
	}
	*formatp = format;
	return (gfarm_iobuffer_get_error(conn->sendbuffer));
}

gfarm_error_t
gfp_xdr_vsend_ref_size_add(size_t *sizep, const char **formatp, va_list *app)
{
	const char *format = *formatp;
	size_t size = *sizep;
	gfarm_int32_t i, n;
	gfarm_uint32_t lv[2];
#if INT64T_IS_FLOAT
	int minus;
#endif

#ifndef __KERNEL__      /* double */
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nd;
#else
	double d;
#	define nd d
#endif
#endif
	const char **sp, *s;
	size_t *szp;

	for (; *format; format++) {
		switch (*format) {
		case 'c':
			(void)va_arg(*app, gfarm_int8_t *);
			size += sizeof(gfarm_int8_t);
			continue;
		case 'h':
			(void)va_arg(*app, gfarm_int16_t *);
			size += sizeof(gfarm_int16_t);
			continue;
		case 'i':
			(void)va_arg(*app, gfarm_int32_t *);
			size += sizeof(gfarm_int32_t);
			continue;
		case 'l':
			/*
			 * note that because actual type of gfarm_int64_t
			 * may be diffenent (int64_t or double), we use lv here
			 */
			(void)va_arg(*app, gfarm_int64_t *);
			size += sizeof(lv);
			continue;
		case 's':
			sp = va_arg(*app, const char **);
			s = *sp;
			n = strlen(s);
			size += sizeof(i);
			size += n;
			continue;
		case 'S':
			gflog_fatal(GFARM_MSG_1003690,
			    "gfp_xdr_vsend_ref_size_add: unimplemented "
			    "format 'S'");
		case 'b':
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			(void)va_arg(*app, size_t); /* this is ignored */
			szp = va_arg(*app, size_t *);
			n = *szp;
			i = htonl(n);
			size += sizeof(i);
			size += n;
			continue;
		case 'r':
			gflog_fatal(GFARM_MSG_1003691,
			    "gfp_xdr_vsend_ref_size_add: unimplemented "
			    "format 'r'");
			continue;
		case 'f':
#ifndef __KERNEL__      /* double */
			(void)va_arg(*app, double *);
			size += sizeof(nd);
			continue;
#else /* __KERNEL__ */
			gflog_fatal(GFARM_MSG_1003692,
				"floating format is not "
				"supported. '%s'", *formatp);
                        return (GFARM_ERR_PROTOCOL);  /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			gflog_fatal(GFARM_MSG_1003693,
			    "gfp_xdr_vsend_ref_size_add: unimplemented "
			    "format '%c'",
			    *format);
			break;
		}

		break;
	}
	*sizep = size;
	*formatp = format;
	return (GFARM_ERR_NO_ERROR);
}


/*
 * gfp_xdr_vsend_ref() does almost same thing with gfp_xdr_vsend(),
 * only difference is that its parameter format is same with gfp_xdr_vrecv().
 * i.e. gfp_xdr_vsend_ref() takes references to parameter variables,
 * instead of values like what gfp_xdr_vsend() deos.
 */
gfarm_error_t
gfp_xdr_vsend_ref(struct gfp_xdr *conn,
	const char **formatp, va_list *app)
{
	const char *format = *formatp;
	gfarm_int8_t *cp;
	gfarm_int16_t *hp, h;
	gfarm_int32_t *ip, i, n;
	gfarm_int64_t *op, o;
	gfarm_uint32_t lv[2];
#if INT64T_IS_FLOAT
	int minus;
#endif
#ifndef __KERNEL__	/* double */
	double *dp, d;
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nd;
#else
#	define nd d
#endif
#endif /* __KERNEL__ */
	const char **sp, *s;
	size_t *szp;

	for (; *format; format++) {
		switch (*format) {
		case 'c':
			cp = va_arg(*app, gfarm_int8_t *);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    cp, sizeof(*cp));
			continue;
		case 'h':
			hp = va_arg(*app, gfarm_int16_t *);
			h = htons(*hp);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &h, sizeof(h));
			continue;
		case 'i':
			ip = va_arg(*app, gfarm_int32_t *);
			i = htonl(*ip);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			continue;
		case 'l':
			/*
			 * note that because actual type of gfarm_int64_t
			 * may be diffenent (int64_t or double), we must
			 * not pass this as is via network.
			 */
			op = va_arg(*app, gfarm_int64_t *);
			o = *op;
#if INT64T_IS_FLOAT
			minus = o < 0;
			if (minus)
				o = -o;
			lv[0] = o / POWER2_32;
			lv[1] = o - lv[0] * POWER2_32;
			if (minus) {
				lv[0] = ~lv[0];
				lv[1] = ~lv[1];
				if (++lv[1] == 0)
					++lv[0];
			}
#else
			lv[0] = o >> 32;
			lv[1] = o;
#endif
			lv[0] = htonl(lv[0]);
			lv[1] = htonl(lv[1]);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    lv, sizeof(lv));
			continue;
		case 's':
			sp = va_arg(*app, const char **);
			s = *sp;
			n = strlen(s);
			i = htonl(n);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'S':
			gflog_fatal(GFARM_MSG_1003694,
			    "gfp_xdr_vsend_ref: unimplemented format 'S'");
			continue;
		case 'b':
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			(void)va_arg(*app, size_t); /* this is ignored */
			szp = va_arg(*app, size_t *);
			n = *szp;
			i = htonl(n);
			s = va_arg(*app, const char *);
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &i, sizeof(i));
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    s, n);
			continue;
		case 'B':
			gflog_fatal(GFARM_MSG_1003695,
			    "gfp_xdr_vsend_ref: unimplemented format 'B'");
			continue;
		case 'r':
			gflog_fatal(GFARM_MSG_1003696,
			    "gfp_xdr_vsend_ref: unimplemented format 'r'");
			continue;
		case 'f':
#ifndef __KERNEL__	/* double */
			dp = va_arg(*app, double *);
			d = *dp;
#ifndef WORDS_BIGENDIAN
			swab(&d, &nd, sizeof(nd));
#endif
			gfarm_iobuffer_put_write(conn->sendbuffer,
			    &nd, sizeof(nd));
			continue;
#else
			gflog_fatal(GFARM_MSG_1003697, "floating format is not "
				"supported. '%s'", *formatp);
			return (GFARM_ERR_PROTOCOL);  /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			gflog_fatal(GFARM_MSG_1003698,
			    "gfp_xdr_vsend_ref: unimplemented format '%c'",
			    *format);
			break;
		}

		break;
	}
	*formatp = format;
	return (gfarm_iobuffer_get_error(conn->sendbuffer));
}

static gfarm_error_t
recv_sized(struct gfp_xdr *conn, int just, int do_timeout, void *p, size_t sz,
	size_t *sizep)
{
	gfarm_error_t e;
	int rv;

	if (*sizep < sz) {
		gflog_debug(GFARM_MSG_1001001, "recv_size: "
		    "%d bytes expected, but only %d bytes remains",
		    (int)sz, (int)*sizep);
		return (GFARM_ERR_PROTOCOL);  /* too short message */
	}
	rv = gfarm_iobuffer_get_read_x(conn->recvbuffer, p, sz, just,
	    do_timeout);
	*sizep -= rv;
	if (rv != sz) {
		gflog_debug(GFARM_MSG_1001002, "recv_size: "
		    "%d bytes expected, but only %d bytes read",
		    (int)sz, rv);
		e = gfarm_iobuffer_get_error(conn->recvbuffer);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		if (rv == 0) /* maybe usual EOF */
			return (GFARM_ERR_UNEXPECTED_EOF);
		return (GFARM_ERR_PROTOCOL);	/* really unexpected EOF */
	}
	return (GFARM_ERR_NO_ERROR); /* rv may be 0, if sz == 0 */
}

static void
gfp_xdr_vrecv_free(int format_parsed, const char *format, va_list *app)
{
	gfarm_int8_t *cp;
	gfarm_int16_t *hp;
	gfarm_int32_t *ip;
	gfarm_int64_t *op;
#ifndef __KERNEL__      /* double */
	double *dp;
#endif
	char **sp, *s;
	size_t *szp, sz;

	for (; --format_parsed >= 0 && *format; format++) {
		switch (*format) {
		case 'c':
			cp = va_arg(*app, gfarm_int8_t *);
			(void)cp;
			continue;
		case 'h':
			hp = va_arg(*app, gfarm_int16_t *);
			(void)hp;
			continue;
		case 'i':
			ip = va_arg(*app, gfarm_int32_t *);
			(void)ip;
			continue;
		case 'l':
			op = va_arg(*app, gfarm_int64_t *);
			(void)op;
			continue;
		case 'r':
			sz = va_arg(*app, size_t);
			(void)sz;
			szp = va_arg(*app, size_t *);
			(void)szp;
			s = va_arg(*app, char *);
			(void)s;
			continue;
		case 's':
			sp = va_arg(*app, char **);
			free(*sp);
			continue;
		case 'b':
			sz = va_arg(*app, size_t);
			(void)sz;
			szp = va_arg(*app, size_t *);
			(void)szp;
			s = va_arg(*app, char *);
			(void)s;
			continue;
		case 'B':
			szp = va_arg(*app, size_t *);
			(void)szp;
			sp = va_arg(*app, char **);
			free(*sp);
			continue;
		case 'f':
#ifndef __KERNEL__      /* double */
			dp = va_arg(*app, double *);
			(void)dp;
			continue;
#else /* __KERNEL__ */
			gflog_fatal(GFARM_MSG_1003699,
				"floating format is not supported. '%s'",
				format);
			return; /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			break;
		}

		break;
	}
}

gfarm_error_t
gfp_xdr_vrecv_sized_x(struct gfp_xdr *conn, int just, int do_timeout,
	size_t *sizep, int *eofp, const char **formatp, va_list *app)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR, e_save = GFARM_ERR_NO_ERROR;
	const char *format = *formatp;
	gfarm_int8_t *cp;
	gfarm_int16_t *hp;
	gfarm_int32_t *ip, i;
	gfarm_int64_t *op;
	gfarm_uint32_t lv[2];
#if INT64T_IS_FLOAT
	int minus;
#endif
#ifndef __KERNEL__	/* double */
	double *dp;
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nd;
#endif
#endif /* __KERNEL__ */
	char **sp, *s;
	size_t *szp, sz;
	size_t size;
	int overflow = 0;

	int format_parsed = 0;
	const char *format_start = *formatp;
	va_list ap_start;

	va_copy(ap_start, *app);

	if (sizep != NULL)
		size = *sizep;
	else
		size = SIZE_MAX;

	/* do not call gfp_xdr_flush() here for a compound procedure */
	*eofp = 1;

	for (; *format; format++) {
		switch (*format) {
		case 'c':
			cp = va_arg(*app, gfarm_int8_t *);
			if ((e = recv_sized(conn, just, do_timeout, cp,
			    sizeof(*cp), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			format_parsed++;
			continue;
		case 'h':
			hp = va_arg(*app, gfarm_int16_t *);
			if ((e = recv_sized(conn, just, do_timeout, hp,
			    sizeof(*hp), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			*hp = ntohs(*hp);
			format_parsed++;
			continue;
		case 'i':
			ip = va_arg(*app, gfarm_int32_t *);
			if ((e = recv_sized(conn, just, do_timeout, ip,
			    sizeof(*ip), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			*ip = ntohl(*ip);
			format_parsed++;
			continue;
		case 'l':
			op = va_arg(*app, gfarm_int64_t *);
			/*
			 * note that because actual type of gfarm_int64_t
			 * may be diffenent (int64_t or double), we must
			 * not pass this as is via network.
			 */
			if ((e = recv_sized(conn, just, do_timeout, lv,
			    sizeof(lv), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			lv[0] = ntohl(lv[0]);
			lv[1] = ntohl(lv[1]);
#if INT64T_IS_FLOAT
			minus = lv[0] & 0x80000000;
			if (minus) {
				lv[0] = ~lv[0];
				lv[1] = ~lv[1];
				if (++lv[1] == 0)
					++lv[0];
			}
			*op = lv[0] * POWER2_32 + lv[1];
			if (minus)
				*op = -*op;
#else
				*op = ((gfarm_int64_t)lv[0] << 32) | lv[1];
#endif
			format_parsed++;
			continue;
		case 'r':
			sz = va_arg(*app, size_t);
			szp = va_arg(*app, size_t *);
			s = va_arg(*app, char *);
			if ((e = recv_sized(conn, just, do_timeout, s,
			    sz, szp)) != GFARM_ERR_NO_ERROR)
				break;
			continue;
		case 's':
			sp = va_arg(*app, char **);
			if ((e = recv_sized(conn, just, do_timeout, &i,
			    sizeof(i), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			i = ntohl(i);
			sz = gfarm_size_add(&overflow, i, 1);
			if (overflow) {
				e = GFARM_ERR_PROTOCOL;
				break;
			}
			GFARM_MALLOC_ARRAY(*sp, sz);
			format_parsed++;
			if (*sp == NULL) {
				if ((e = gfp_xdr_purge_sized(conn, just,
				    sz, &size)) != GFARM_ERR_NO_ERROR)
					break;
				e_save = GFARM_ERR_NO_MEMORY;
				continue;
			}
			if ((e = recv_sized(conn, just, do_timeout, *sp, i,
			    &size)) != GFARM_ERR_NO_ERROR)
				break;
			(*sp)[i] = '\0';
			continue;
		case 'b':
			sz = va_arg(*app, size_t);
			szp = va_arg(*app, size_t *);
			s = va_arg(*app, char *);
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			if ((e = recv_sized(conn, just, do_timeout, &i,
			    sizeof(i), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			i = ntohl(i);
			*szp = i;
			if (i <= sz) {
				if ((e = recv_sized(conn, just, do_timeout, s,
				    i, &size)) != GFARM_ERR_NO_ERROR)
					break;
			} else {
				if (size < i) {
					e = GFARM_ERR_PROTOCOL;
					break;
				}
				if ((e = recv_sized(conn, just, do_timeout, s,
				    sz, &size)) != GFARM_ERR_NO_ERROR)
					break;
				/* abandon (i - sz) bytes */
				if ((e = gfp_xdr_purge_sized(conn, just,
				    i - sz, &size)) != GFARM_ERR_NO_ERROR)
					break;
			}
			format_parsed++;
			continue;
		case 'B':
			szp = va_arg(*app, size_t *);
			sp = va_arg(*app, char **);
			/*
			 * note that because actual type of size_t may be
			 * diffenent ([u]int32_t or [u]int64_t), we must not
			 * pass this as is via network.
			 */
			if ((e = recv_sized(conn, just, do_timeout, &i,
			    sizeof(i), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
			i = ntohl(i);
			*szp = i;
			/* XXX is this +1 really necessary? */
			sz = gfarm_size_add(&overflow, i, 1);
			if (overflow) {
				e = GFARM_ERR_PROTOCOL;
				break;
			}
			GFARM_MALLOC_ARRAY(*sp, sz);
			format_parsed++;
			if (*sp == NULL) {
				if ((e = gfp_xdr_purge_sized(conn, just,
				    sz, &size)) != GFARM_ERR_NO_ERROR)
					break;
				e_save = GFARM_ERR_NO_MEMORY;
				continue;
			}
			if ((e = recv_sized(conn, just, do_timeout, *sp, i,
			    &size)) != GFARM_ERR_NO_ERROR)
				break;
			continue;
		case 'f':
#ifndef __KERNEL__	/* double */
			dp = va_arg(*app, double *);
			assert(sizeof(*dp) == 8);
			if ((e = recv_sized(conn, just, do_timeout, dp,
			    sizeof(*dp), &size)) != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gfp_xdr_vrecv_free(format_parsed,
					    format_start, &ap_start);
					return (GFARM_ERR_NO_ERROR); /* EOF */
				}
				break;
			}
#ifndef WORDS_BIGENDIAN
			swab(dp, &nd, sizeof(nd));
			*dp = *(double *)&nd;
#endif
			format_parsed++;
			continue;
#else /* __KERNEL__ */
			gflog_fatal(GFARM_MSG_1003700, "floating format is not "
				"supported. '%s'", *formatp);
			return (GFARM_ERR_PROTOCOL);  /* floating */
#endif /* __KERNEL__ */
		case '/':
			break;

		default:
			gflog_fatal(GFARM_MSG_1003701,
			    "gfp_xdr_vrecv_sized_x: unimplemented format '%c'",
			    *format);
			break;
		}

		break;
	}
	if (sizep != NULL)
		*sizep = size;
	*formatp = format;
	*eofp = 0;

	/* connection error has most precedence to avoid protocol confusion */
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_vrecv_free(format_parsed, format_start, &ap_start);
		return (e);
	}

	/* iobuffer error may be a connection error */
	if ((e = gfarm_iobuffer_get_error(conn->recvbuffer)) !=
	    GFARM_ERR_NO_ERROR) {
		gfp_xdr_vrecv_free(format_parsed, format_start, &ap_start);
		return (e);
	}

	if (e_save != GFARM_ERR_NO_ERROR)
		gfp_xdr_vrecv_free(format_parsed, format_start, &ap_start);

	return (e_save); /* NO_MEMORY or SUCCESS */
}

gfarm_error_t
gfp_xdr_vrecv_sized(struct gfp_xdr *conn, int just, int do_timeout,
	size_t *sizep, int *eofp, const char **formatp, va_list *app)
{
	return (gfp_xdr_vrecv_sized_x(conn, just, do_timeout,
	    sizep, eofp, formatp, app));
}

gfarm_error_t
gfp_xdr_vrecv(struct gfp_xdr *conn, int just, int do_timeout,
	int *eofp, const char **formatp, va_list *app)
{
	return (gfp_xdr_vrecv_sized_x(conn, just, do_timeout,
	    NULL, eofp, formatp, app));
}

gfarm_error_t
gfp_xdr_send_size_add(size_t *sizep, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vsend_size_add(sizep, &format, &ap);
	va_end(ap);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (*format != '\0') {
		gflog_debug(GFARM_MSG_1001003, "gfp_xdr_send_size_add: "
		    "invalid format character: %c(%x)", *format, *format);
		return (GFARM_ERRMSG_GFP_XDR_SEND_INVALID_FORMAT_CHARACTER);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_send(struct gfp_xdr *conn, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vsend(conn, &format, &ap);
	va_end(ap);

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001004,
			"gfp_xdr_vsend() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (*format != '\0') {
		gflog_debug(GFARM_MSG_1001005, "gfp_xdr_send_size: "
		    "invalid format character: %c(%x)", *format, *format);
		return (GFARM_ERRMSG_GFP_XDR_SEND_INVALID_FORMAT_CHARACTER);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_uint32_t
gfp_xdr_send_calc_crc32(struct gfp_xdr *conn, gfarm_uint32_t crc, int offset,
	size_t length)
{
	return (gfarm_iobuffer_calc_crc32(conn->sendbuffer, crc,
		offset, length, 0));
}

gfarm_uint32_t
gfp_xdr_recv_calc_crc32(struct gfp_xdr *conn, gfarm_uint32_t crc, int offset,
	size_t length)
{
	return (gfarm_iobuffer_calc_crc32(conn->recvbuffer, crc,
		offset, length, 1));
}

gfarm_uint32_t
gfp_xdr_recv_get_crc32_ahead(struct gfp_xdr *conn, int offset)
{
	gfarm_uint32_t n;
	int err;
	int len = gfarm_iobuffer_get_read_x_ahead(conn->recvbuffer,
		&n, sizeof(n), 1, 1, offset, &err);

	if (len != sizeof(n))
		return (0);
	return (ntohl(n));
}

static gfarm_error_t
gfp_xdr_vrecv_sized_x_check_format(
	struct gfp_xdr *conn, int just, int do_timeout,
	size_t *sizep, int *eofp, const char *format, va_list *app)
{
	gfarm_error_t e;

	e = gfp_xdr_vrecv_sized_x(conn, just, do_timeout,
	    sizep, eofp, &format, app);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001007,
		    "gfp_xdr_vrecv_sized_x() failed: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (*eofp)
		return (GFARM_ERR_NO_ERROR);
	if (*format != '\0') {
		gflog_debug(GFARM_MSG_1001008,
		    "gfp_xdr_vrecv_sized_x_check_format(): "
		    "invalid format character: %c(%x)", *format, *format);
		return (GFARM_ERRMSG_GFP_XDR_RECV_INVALID_FORMAT_CHARACTER);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_recv_sized(struct gfp_xdr *conn, int just, int do_timeout,
	size_t *sizep, int *eofp, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrecv_sized_x_check_format(conn, just, do_timeout,
	    sizep, eofp, format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfp_xdr_recv(struct gfp_xdr *conn, int just,
	int *eofp, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrecv_sized_x_check_format(conn, just, 1,
	    NULL, eofp, format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfp_xdr_recv_notimeout(struct gfp_xdr *conn, int just,
	int *eofp, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrecv_sized_x_check_format(conn, just, 0,
	    NULL, eofp, format, &ap);
	va_end(ap);
	return (e);
}

/* this function is used to read from file. i.e. server/gfmd/journal_file.c */
gfarm_error_t
gfp_xdr_recv_ahead(struct gfp_xdr *conn, int len, size_t *availp)
{
	int r = gfarm_iobuffer_read_ahead(conn->recvbuffer, len);

	if (r == 0) {
		*availp = 0;
		return (gfarm_iobuffer_get_error(conn->recvbuffer));
	}
	*availp = gfarm_iobuffer_avail_length(conn->recvbuffer);
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
gfp_xdr_vrpc_send_begin(struct gfp_xdr *conn,
	gfarm_int32_t xid_and_type, int *size_posp,
	gfarm_int32_t i, const char **formatp, va_list *app)
{
	gfarm_error_t e;
	int size_pos;

	gfp_xdr_begin_sendbuffer_pindown(conn);

	e = gfp_xdr_send(conn, ASYNC_REQUEST_HEADER_FORMAT_TYPE_XID,
	    xid_and_type);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_end_sendbuffer_pindown(conn);
		return (e);
	}
	/* save this position -> size_pos */
	gfp_xdr_sendbuffer_get_pos(conn, &size_pos);
	e = gfp_xdr_send(conn, ASYNC_REQUEST_HEADER_FORMAT_SIZE,
	    (gfarm_int32_t)0);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_end_sendbuffer_pindown(conn);
		return (e);
	}

	e = gfp_xdr_send(conn, "i", i);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_end_sendbuffer_pindown(conn);
		gflog_debug(GFARM_MSG_1003702,
		    "sending command/errcode (%d) failed: %s",
		    i, gfarm_error_string(e));
		return (e);
	}
	e = gfp_xdr_vsend(conn, formatp, app);
	if (e != GFARM_ERR_NO_ERROR) {
		gfp_xdr_end_sendbuffer_pindown(conn);
		gflog_debug(GFARM_MSG_1003703,
		    "sending parameter (%d) failed: %s",
		    i, gfarm_error_string(e));
		return (e);
	}
	*size_posp = size_pos;
	return (GFARM_ERR_NO_ERROR);
}
	

gfarm_error_t
gfp_xdr_vrpc_send_request_begin(struct gfp_xdr *conn,
	gfp_xdr_xid_t xid, int *size_posp,
	gfarm_int32_t command,
	const char **formatp, va_list *app)
{
	return (gfp_xdr_vrpc_send_begin(conn,
	    xid | XID_TYPE_REQUEST, size_posp, command, formatp, app));
}
	
gfarm_error_t
gfp_xdr_vrpc_send_result_begin(struct gfp_xdr *conn,
	gfp_xdr_xid_t xid, int *size_posp,
	gfarm_int32_t errcode, const char **formatp, va_list *app)
{
	const char *fmt = "";

	if (errcode == 0)
		return (gfp_xdr_vrpc_send_begin(conn,
		    xid | XID_TYPE_RESULT, size_posp, errcode, formatp, app));
	else
		return (gfp_xdr_vrpc_send_begin(conn,
		    xid | XID_TYPE_RESULT, size_posp, errcode, &fmt, NULL));
}

gfarm_error_t
gfp_xdr_rpc_send_result_begin(struct gfp_xdr *conn,
	gfp_xdr_xid_t xid, int *size_posp,
	gfarm_int32_t errcode, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrpc_send_result_begin(conn, xid, size_posp,
	    errcode, &format, &ap);
	va_end(ap);
	return (e);
}

void
gfp_xdr_rpc_send_end(struct gfp_xdr *conn, int size_pos)
{
	int current_pos;
	gfarm_int32_t size;

	gfp_xdr_sendbuffer_get_pos(conn, &current_pos);
	size = current_pos - size_pos - ASYNC_REQUEST_HEADER_SIZE_SIZE;
	size = ntohl(size);
	gfp_xdr_sendbuffer_overwrite_at(conn,
	    &size, ASYNC_REQUEST_HEADER_SIZE_SIZE, size_pos);
	gfp_xdr_end_sendbuffer_pindown(conn);
}

/*
 * do RPC request
 */
#if 0
gfarm_error_t
gfp_xdr_vrpc_request(struct gfp_xdr *conn, gfarm_int32_t command,
	const char **formatp, va_list *app)
{
	return (gfp_xdr_vrpc_request_with_ref(conn, command,
	    formatp, app, 0));
}
#endif

gfarm_error_t
gfp_xdr_vrpc_request_with_ref(struct gfp_xdr *conn, gfarm_int32_t command,
	const char **formatp, va_list *app, int isref)
{
	gfarm_error_t e;

	/*
	 * send request
	 */
	e = gfp_xdr_send(conn, "i", command);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001009,
			"sending command (%d) failed: %s",
			command,
			gfarm_error_string(e));
		return (e);
	}
	return ((isref ? gfp_xdr_vsend_ref : gfp_xdr_vsend)
	    (conn, formatp, app));
}

/*
 * used by client side of both synchronous and asynchronous protocol.
 * if sizep == NULL, it's a synchronous protocol, otherwise asynchronous.
 * Note that this function assumes that async_header is already received.
 *
 * Callers of this function should check the followings:
 *	return value == GFARM_ERR_NOERROR
 *	*wrapping_errorp == GFARM_ERR_NOERROR, if wrapping_format != NULL
 *	*errorp == GFARM_ERR_NOERROR
 * And if there is no remaining output parameter:
 *	*sizep == 0
 */
gfarm_error_t
gfp_xdr_vrpc_wrapped_result_sized(
	struct gfp_xdr *conn, int just, int do_timeout,
	size_t *sizep, gfarm_int32_t *wrapping_errorp,
	const char *wrapping_format, va_list *wrapping_app,
	gfarm_int32_t *errorp, const char **formatp, va_list *app)
{
	gfarm_error_t e;
	int eof;

	/*
	 * receive response
	 */

	/* always do timeout here, because async header is already received */
	if (wrapping_format != NULL) {
		if ((e = gfp_xdr_recv_sized(conn, just, 1, sizep, &eof,
		    "i", wrapping_errorp)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1003704,
			    "receiving wrapping response (%d) failed: %s",
			    just, gfarm_error_string(e));
			return (e);
		}
		if (eof) { /* rpc status missing */
			gflog_debug(GFARM_MSG_1003705,
			    "Unexpected EOF when receiving wrapping response");
			return (GFARM_ERR_UNEXPECTED_EOF);
		}
		if (*wrapping_errorp != GFARM_ERR_NO_ERROR)
			return (GFARM_ERR_NO_ERROR);
		if ((e = gfp_xdr_vrecv_sized(conn, just, 1, sizep, &eof,
		    &wrapping_format, wrapping_app)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1003706,
			    "receiving wrapping arguments: %s",
			    gfarm_error_string(e));
			return (e);
		}
	}

	/* timeout if it's asynchronous protocol, or do_timeout is specified */
	if ((e = gfp_xdr_recv_sized(conn, just,
	    wrapping_format != NULL || do_timeout,
	    sizep, &eof, "i", errorp)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001010,
			"receiving response (%d) failed: %s",
			just, gfarm_error_string(e));
		return (e);
	}

	if (eof) { /* rpc status missing */
		gflog_debug(GFARM_MSG_1001011,
			"Unexpected EOF when receiving response: %s",
			gfarm_error_string(GFARM_ERR_UNEXPECTED_EOF));
		return (GFARM_ERR_UNEXPECTED_EOF);
	}
	if (*errorp != GFARM_ERR_NO_ERROR)
		return (GFARM_ERR_NO_ERROR);

	/* always do timeout here, because error code is already received */
	e = gfp_xdr_vrecv_sized_x(conn, just, 1, sizep, &eof, formatp, app);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001012,
			"gfp_xdr_vrecv_sized_x() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (eof) { /* rpc return value missing */
		gflog_debug(GFARM_MSG_1001013,
			"Unexpected EOF when doing xdr vrecv: %s",
			gfarm_error_string(GFARM_ERR_UNEXPECTED_EOF));
		return (GFARM_ERR_UNEXPECTED_EOF);
	}
	if (**formatp != '\0') {
		gflog_debug(GFARM_MSG_1003707,
		    "invalid format character: %c(%x)", **formatp, **formatp);
		return (GFARM_ERRMSG_GFP_XDR_VRPC_INVALID_FORMAT_CHARACTER);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_vrpc_result_sized(struct gfp_xdr *conn, int just, size_t *sizep,
	gfarm_int32_t *errorp, const char **formatp, va_list *app)
{
	/*
	 * always do timeout, because this is only called
	 * after asynchronous protocol header is received
	 */
	return (gfp_xdr_vrpc_wrapped_result_sized(conn, just, 1,
	    sizep, NULL, NULL, NULL, errorp, formatp, app));
}

#if 0
/*
 * get RPC result
 */
gfarm_error_t
gfp_xdr_vrpc_result(struct gfp_xdr *conn, int just, int do_timeout,
	gfarm_int32_t *errorp, const char **formatp, va_list *app)
{
	return (gfp_xdr_vrpc_wrapped_result_sized(conn, just, do_timeout,
	    NULL, errorp, NULL, NULL, formatp, app));
}

/*
 * do RPC with "request-args/result-args" format string.
 */
gfarm_error_t
gfp_xdr_vrpc(struct gfp_xdr *conn, int just, int do_timeout,
	gfarm_int32_t command, gfarm_int32_t *errorp,
	const char **formatp, va_list *app)
{
	gfarm_error_t e;

	/*
	 * send request
	 */
	e = gfp_xdr_vrpc_request(conn, command, formatp, app);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfp_xdr_flush(conn);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001015,
			"gfp_xdr_vrpc_request() failed: %s",
			gfarm_error_string(e));
		return (e);
	}

	if (**formatp != '/') {
#if 1
		gflog_fatal(GFARM_MSG_1000018, "%s",
		    gfarm_error_string(GFARM_ERRMSG_GFP_XDR_VRPC_MISSING_RESULT_IN_FORMAT_STRING));
#endif
		return (GFARM_ERRMSG_GFP_XDR_VRPC_MISSING_RESULT_IN_FORMAT_STRING);
	}
	(*formatp)++;

	return (gfp_xdr_vrpc_result(conn, just, do_timeout,
	    errorp, formatp, app));
}
#endif

/*
 * low level interface, this does not wait to receive desired length.
 */
gfarm_error_t
gfp_xdr_recv_partial(struct gfp_xdr *conn, int just, void *data, int length,
	int *receivedp)
{
	gfarm_error_t e;
	int received = gfarm_iobuffer_get_read_partial_x(
	    conn->recvbuffer, data, length, just, 1);

	if (received == 0 && (e = gfarm_iobuffer_get_error(conn->recvbuffer))
	    != GFARM_ERR_NO_ERROR)
		return (e);

	*receivedp = received;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_recv_get_error(struct gfp_xdr *conn)
{
	return (gfarm_iobuffer_get_error(conn->recvbuffer));
}

void
gfp_xdr_begin_sendbuffer_pindown(struct gfp_xdr *conn)
{
	gfarm_iobuffer_begin_pindown(conn->sendbuffer);
}

void
gfp_xdr_end_sendbuffer_pindown(struct gfp_xdr *conn)
{
	gfarm_iobuffer_end_pindown(conn->sendbuffer);
}

void
gfp_xdr_sendbuffer_get_pos(struct gfp_xdr *conn, int *posp)
{
	gfarm_iobuffer_get_pos(conn->sendbuffer, posp);
}

void
gfp_xdr_sendbuffer_overwrite_at(struct gfp_xdr *conn,
	const void *data, int len, int pos)
{
	gfarm_iobuffer_overwrite_at(conn->sendbuffer, data, len, pos);
}

/* For bayesian Cooperative Cache */
#define SQL_CALLBACK_WRAPPER_STRUCTURE(tag)		\
struct wrap_ ## tag ## _entry {					\
	int valid_num_of_entry;						\
	int allocated_entries_size;					\
	struct tag##_entry *entries;				\
}

#define STRUCT_SQL_CALLBACK_WRAPPER(tag) \
	struct wrap_ ## tag ## _entry

#define SQL_CALLBACK_WRAPPER_GARD_ARYSIZE(_wp, increment_size)			\
	do {																\
		if ((_wp)->allocated_entries_size < (_wp)->valid_num_of_entry) { \
			(_wp)->allocated_entries_size += (increment_size);			\
			(_wp)->entries = GFARM_REALLOC_ARRAY((_wp)->entries, (_wp)->entries, \
												 (_wp)->allocated_entries_size); \
		}																\
	} while (0)

#define P_GET_SQL_WRAPPER_CURRENT_ARY_LEN(p) \
	((p)->valid_num_of_entry)

#define P_INCREMENT_SQL_WRAPPER_ARY_LEN(p) \
	((p)->valid_num_of_entry++)

#define P_SQL_WRAPPER_ENTRY(p) \
	((p)->entries)

#define GET_SQL_WRAPPER_CURRENT_ARY_LEN(val) \
	((val).valid_num_of_entry)

#define INCREMENT_SQL_WRAPPER_ARY_LEN(val) \
	((val).valid_num_of_entry++)

#define SQL_WRAPPER_ENTRY(val) \
	((val).entries)

void
gfp_record_client_subquery(sqlite3 *db, struct gfp_xdr *conn, 
	struct sockaddr *client_addr)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)client_addr;
	gfarm_uint32_t client_ip = (gfarm_uint32_t)sin->sin_addr.s_addr;
	char *errmsg = NULL;
	int rc;
	char sql[255];
	char *_sql = 
		"insert or ignore into clients(cliaddr, total_reads, total_hits) "
		"          values(%d, 0, 0)";
	
	snprintf(sql, sizeof(sql), _sql, client_ip);
	
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
			"Could not record a client into the db %s (client ip: %d)",
			errmsg, client_ip);
		sqlite3_free(errmsg);
	} else 
		gflog_debug(GFARM_MSG_1000018,
					"insert a client (%d) into the table\n", client_ip);
	
	conn->client_addr     = client_ip;	
	conn->total_cache_hit = 0;
	conn->total_read      = 0;
	
	return;
}

void
gfp_record_client(sqlite3 *db, struct gfp_xdr *conn, 
	struct sockaddr *client_addr)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)client_addr;
	gfarm_uint32_t client_ip = (gfarm_uint32_t)sin->sin_addr.s_addr;
	char *errmsg = NULL;
	int rc;
	char sql[255];
	char *_sql = 
		/* "insert or ignore into clients(cliaddr, total_reads, total_hits) " */
		/* "          values(%d, 10, 10)"; */
		"	INSERT or IGNORE into clients (cliaddr, total_reads, total_hits)"
		"		SELECT coalesce(cliaddr, %u), "
		"		       coalesce(total_reads, 0), "
		"		       coalesce(total_hits, 0)"
		"		FROM clients where cliaddr = %u";
	
	snprintf(sql, sizeof(sql), _sql, client_ip, client_ip);
	
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
			"Could not record a client into the db %s (client ip: %d)",
			errmsg, client_ip);
		sqlite3_free(errmsg);
	} else 
		gflog_debug(GFARM_MSG_1000018,
					"insert a client (%d) into the table\n", client_ip);
	
	conn->client_addr     = client_ip;	
	conn->total_cache_hit = 0;
	conn->total_read      = 0;
	
	return;
}

void
gfp_update_histgram_entries(sqlite3 *db, struct gfp_xdr *conn, 
	gfarm_int64_t ino, gfarm_int64_t pagenum)
{
	int rc;
	char *errmsg = NULL;
	char sql[255];
	char *_sql = 
		"INSERT INTO entries (inum, pagenum)"
		"       SELECT %lu, %lu"
		"              FROM dual"
		"                   WHERE NOT EXISTS "
		"      (SELECT id FROM entries WHERE inum = %lu AND pagenum = %lu)";

	snprintf(sql, sizeof(sql), _sql, ino, pagenum, ino, pagenum);
	
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
					"Could not update histgram entries: %s", errmsg);
		sqlite3_free(errmsg);
	}	
}

gfarm_error_t
gfp_update_totalchit_and_total_read(sqlite3 *db, struct gfp_xdr *conn)
{	
	int rc;
	char *errmsg = NULL;
	char sql[1024];
	char *_sql = 
		"INSERT into clients (cliaddr, total_reads, total_hits)"
		"       SELECT coalesce(cliaddr, %u), "
		"              coalesce(total_reads + %u, %u), "
		"              coalesce(total_hits + %u, %u)"
		"                      FROM clients where cliaddr = %u";

	snprintf(sql, sizeof(sql), _sql, conn->client_addr, 
			 conn->total_read, conn->total_read,
			 conn->total_cache_hit, conn->total_cache_hit,
			 conn->client_addr);
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
		   "Could not update cache hit rate and total read: %s", errmsg);
		sqlite3_free(errmsg);
		return GFARM_ERR_SQL;
	}
	
	return GFARM_ERR_NO_ERROR;
}

struct conn_entry {
	gfarm_uint32_t cliaddr;
	gfarm_uint32_t total_reads;
	gfarm_uint32_t total_hits;
};
SQL_CALLBACK_WRAPPER_STRUCTURE(conn);

static int
callback_form_conn_entries(void *ptr, int argc, 
	char **argv, char **azColName)
{
	int i;
	int idx;
	STRUCT_SQL_CALLBACK_WRAPPER(conn) *p = (STRUCT_SQL_CALLBACK_WRAPPER(conn)*)ptr;
	
	SQL_CALLBACK_WRAPPER_GARD_ARYSIZE(p, 10);
	
	P_INCREMENT_SQL_WRAPPER_ARY_LEN(p);
	idx = P_GET_SQL_WRAPPER_CURRENT_ARY_LEN(p) - 1;

	for (i = 0 ; i < argc ; i++) {
		if (strcmp(azColName[i], "cliaddr") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].cliaddr = atoi(argv[i]);
		} else if (strcmp(azColName[i], "total_reads") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].total_reads = atoll(argv[i]);
		} else if (strcmp(azColName[i], "total_hits") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].total_hits = atoll(argv[i]);
		}
	}	

	return SQLITE_OK;
}

gfarm_error_t
gfp_get_totalchit_and_total_read(sqlite3 *db, struct gfp_xdr *conn, 
	const char *diag, char **result, int *result_len)
{
	int rc;
	int i;
	char *s;
	char *errmsg = NULL;
	char tmp[255];
	STRUCT_SQL_CALLBACK_WRAPPER(conn) clients;
	size_t ary_size = 10;
	char *_sql = 
		"SELECT * from clients";

	memset(&clients, 0, sizeof(STRUCT_SQL_CALLBACK_WRAPPER(conn)));
	GFARM_MALLOC_ARRAY(clients.entries, 10);
	clients.allocated_entries_size = 10;
	clients.valid_num_of_entry     = 0;
	if (clients.entries == NULL) {
		gflog_fatal_errno(GFARM_MSG_UNFIXED, "%s: no memory for %d bytes",
						  diag, 10);
	}
	
	rc = sqlite3_exec(db, _sql, callback_form_conn_entries, &clients, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_UNFIXED,
					"Could not obtain clients list: %s", errmsg);
		sqlite3_free(errmsg);
		free(clients.entries);
		return GFARM_ERR_SQL;
	}

	/* if (GET_SQL_WRAPPER_CURRENT_ARY_LEN(clients) > 1) { */
	/* 	gflog_error(GFARM_MSG_1000018, */
	/* 				"client record may not be unique: %s (%d rows)",  */
	/* 				errmsg, */
	/* 				GET_SQL_WRAPPER_CURRENT_ARY_LEN(clients)); */
	/* } */

	ary_size = (clients.valid_num_of_entry * (15 + 10 + 10 + 2)) + 1;
	GFARM_MALLOC_ARRAY(s, ary_size);
	memset(s, 0, ary_size);

	for (i = 0 ; i < clients.valid_num_of_entry ; i++) {
		snprintf(tmp, sizeof(tmp), "%u:%u/%u\n", 
			SQL_WRAPPER_ENTRY(clients)[i].cliaddr, 
			SQL_WRAPPER_ENTRY(clients)[i].total_hits,
			SQL_WRAPPER_ENTRY(clients)[i].total_reads);
		strcat(s, tmp);
	}

	free(clients.entries);
	*result     = s;
	*result_len = strlen(s);


	gflog_error(GFARM_MSG_UNFIXED,"send value (%d clients): %s", 
				clients.valid_num_of_entry, s);
	
	return GFARM_ERR_NO_ERROR;
}

gfarm_error_t
gfp_clear_totalchit_and_total_read(sqlite3 *db, struct gfp_xdr *conn)
{
	int rc;
	char *errmsg = NULL;
	char *_sql = 
		"UPDATE clients SET total_reads = 0, total_hits = 0 WHERE id >= 0";
	char *__sql = 
		"DELETE from reads WHERE id >= 0";
	
	rc = sqlite3_exec(db, _sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
		  "Could not clear total_reads and total_hits: %s", errmsg);
		sqlite3_free(errmsg);
		return GFARM_ERR_SQL;
	}

	rc = sqlite3_exec(db, __sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
		  "Could not delete lru cache buffer: %s", errmsg);
		sqlite3_free(errmsg);
		return GFARM_ERR_SQL;
	}
	

	return GFARM_ERR_NO_ERROR;
}

void
gfp_show_client_hitrates(struct gfp_xdr *conn)
{
	/* TODO: total_cache_hit and total_read is stored into DB */
	double hitrate = (double)conn->total_cache_hit / (double)conn->total_read;
	
	conn->latest_cache_hitrate = hitrate;

	gflog_info(GFARM_MSG_1004204, 
          "############## <IP:%d> CACHE HIT RATE %lf (hit: %u / read: %u) ##############",
		  conn->client_addr, hitrate, conn->total_cache_hit, conn->total_read);
}

void
gfp_show_msg(struct gfp_xdr *conn, const char *msg)
{
	gflog_info(GFARM_MSG_1004205, "%s", msg);
}

/* struct client_entry { */
/* 	gfarm_uint32_t client_addr; */
/* }; */
/* SQL_CALLBACK_WRAPPER_STRUCTURE(client); */

/* void */
/* gfp_get_allclients(struct gfp_xdr *conn, struct wrap_cache_entry *p) */
/* { */
/* 	char *_sql = "SELECT cliaddr FROM clients";	 */

/* 	rc = sqlite3_exec(db, _sql, callback_form_cache_entries, &cache, &errmsg); */
/* 	if (rc != SQLITE_OK) { */
/* 		gflog_error(GFARM_MSG_1000018, */
/* 					"Could not obtain native lru: %s", errmsg); */
/* 		sqlite3_free(errmsg); */
/* 		free(cache.entries); */
/* 		return; */
/* 	}	 */
/* } */

struct cache_entry {
	gfarm_uint32_t client_id;
	gfarm_uint64_t inum;
	gfarm_uint64_t pagenum;
	gfarm_uint32_t count;
};
SQL_CALLBACK_WRAPPER_STRUCTURE(cache);

static int
callback_form_cache_entries(void *ptr, int argc, 
	char **argv, char **azColName)
{
	int i;
	int idx;
	STRUCT_SQL_CALLBACK_WRAPPER(cache) *p = (STRUCT_SQL_CALLBACK_WRAPPER(cache)*)ptr;

	SQL_CALLBACK_WRAPPER_GARD_ARYSIZE(p, 10);

	P_INCREMENT_SQL_WRAPPER_ARY_LEN(p);
	idx = P_GET_SQL_WRAPPER_CURRENT_ARY_LEN(p) - 1;
	
	for (i = 0 ; i < argc ; i++) {
		if (strcmp(azColName[i], "client_id") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].client_id = atoi(argv[i]);
		} else if (strcmp(azColName[i], "inum") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].inum = atoll(argv[i]);
		} else if (strcmp(azColName[i], "pagenum") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].pagenum = atoll(argv[i]);
		} else if (strcmp(azColName[i], "count") == 0) {
			P_SQL_WRAPPER_ENTRY(p)[idx].count = atoll(argv[i]);
		}
	}
	
	return SQLITE_OK;
}

void
gfp_count_client_cachehits_by_naive_lru(sqlite3 *db, struct gfp_xdr *conn,
	gfarm_int64_t ino, gfarm_int64_t offset, size_t size, gfarm_uint64_t granularity,
	gfarm_uint64_t clientcachememsize)
{
	char *errmsg = NULL;
	int rc;
	char sql[255];
	char *_sql2 =
		"SELECT client_id, inum, pagenum, count"
		"      FROM reads where client_id = %u order by id desc limit %llu";
	gfarm_uint64_t i, j;
	size_t arysize = clientcachememsize / granularity;
	STRUCT_SQL_CALLBACK_WRAPPER(cache) cache;

	memset(&cache, 0, sizeof(STRUCT_SQL_CALLBACK_WRAPPER(cache)));
	GFARM_MALLOC_ARRAY(cache.entries, arysize);
	cache.allocated_entries_size = arysize;
	cache.valid_num_of_entry     = 0;

	/* gflog_debug(GFARM_MSG_1004206,  */
	/* 			"update lru list: offset = %lu, size = %lu", offset, size); */

	snprintf(sql, sizeof(sql), _sql2, conn->client_addr, arysize);
	rc = sqlite3_exec(db, sql, callback_form_cache_entries, &cache, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000018,
					"Could not obtain native lru: %s", errmsg);
		sqlite3_free(errmsg);
		free(cache.entries);
		return;
	}

	conn->total_read      = 0;
	conn->total_cache_hit = 0;

	for (i = offset / granularity ; i < ((offset + size) / granularity) + 1 ; i++) {
		for (j = 0 ; j < cache.valid_num_of_entry ; j++) {
			if ((cache.entries[j].inum == ino) &&
				(cache.entries[j].pagenum == i)) {
				/* Cache hit!!! */
				conn->total_cache_hit++;
				j = cache.valid_num_of_entry;

				gflog_info(GFARM_MSG_UNFIXED,
						   "EEEEEEEEEEEEEEEE: %u", 
						   cache.valid_num_of_entry);
			}
		}
		conn->total_read++;
	}

	/* gflog_debug(GFARM_MSG_UNFIXED, "<IP:%d> READ hits: %lu / reads: %lu",  */
	/* 			conn->client_addr, conn->total_cache_hit, conn->total_read); */
	gfp_update_totalchit_and_total_read(db, conn);

	conn->total_read      = 0;
	conn->total_cache_hit = 0;
	
	free(cache.entries);
	return;
}

void
gfp_update_reads_histgram(sqlite3 *db, struct gfp_xdr *conn, 
	gfarm_int64_t ino, gfarm_int64_t offset, 
	size_t size, gfarm_uint64_t granularity)
{
	char *errmsg = NULL;
	gfarm_uint64_t i;
	gfarm_uint64_t count = 0;
	int rc;
	char sql[1024];
	char *_sql = 
		"INSERT into reads (entry_id, client_id, inum, pagenum, count)"
		"       SELECT coalesce(entry_id, LAST_INSERT_ROWID()) , coalesce(client_id, %u), coalesce(inum, %u),"
		"              coalesce(pagenum, %llu), coalesce(max(count) + 1, 1)"
		"                     FROM reads where client_id = %u AND inum = %llu AND pagenum = %llu";

	for (i = offset / granularity; i < ((offset + size) / granularity) + 1 ; i++) {
		snprintf(sql, sizeof(sql), _sql, 
				 conn->client_addr, ino, i, conn->client_addr, ino, i);
		rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
		if (rc != SQLITE_OK) {
			gflog_error(GFARM_MSG_1000018,
						"Could not update reads histgram: %s", errmsg);
			sqlite3_free(errmsg);
		} else
			count++;
	}
}

gfarm_error_t
gfp_client_insert_or_increment_read_count(sqlite3 *db, struct gfp_xdr *conn)
{
	int rc;
	char *errmsg = NULL;
	char sql[1024];
	char *_sql = 
		"INSERT into clients (cliaddr, total_reads, total_hits) "
		"   SELECT coalesce(cliaddr, %u), coalesce(max(total_reads) + 1, 0), coalesce(total_hits, 0) "
		"      FROM clients where cliaddr = %u";

	snprintf(sql, sizeof(sql), _sql, conn->client_addr, conn->client_addr);
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000510,
					"Could not increment total read count: %s", errmsg);
		sqlite3_free(errmsg);
		return GFARM_ERR_UNEXPECTED_EOF;
	}

	return GFARM_ERR_NO_ERROR;
}

gfarm_error_t
gfp_client_insert_or_increment_total_hit(sqlite3 *db, struct gfp_xdr *conn)
{
	int rc;
	char *errmsg = NULL;
	char sql[1024];
	char *_sql =
		"INSERT into clients (cliaddr, total_reads, total_hits) "
		"    SELECT coalesce(cliaddr, %u), coalesce(total_reads, 0), coalesce(max(total_hits) + 1, 0) "
		"       FROM clients where cliaddr = %u";

	snprintf(sql, sizeof(sql), _sql, conn->client_addr, conn->client_addr);
	rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
	if (rc != SQLITE_OK) {
		gflog_error(GFARM_MSG_1000510,
					"Could not increment total hit: %s", errmsg);
		sqlite3_free(errmsg);
		return GFARM_ERR_UNEXPECTED_EOF;
	}

	return GFARM_ERR_NO_ERROR;	
}

void
gfp_create_histgram(sqlite3 **db, const char *dbname)
{
	int rc;
	sqlite3 *db_p;
	
	/* 
	 * for bayesian cache 
	 */
	rc = sqlite3_open(dbname, &db_p);
	if (rc) {
		sqlite3_close(db_p);
		gflog_fatal_errno(GFARM_MSG_1000599, "Could not open database");
	} else {
		int second_chance = 1;
		char *errmsg = NULL;
		char create_clients[] = 
			"create table clients ("
			"                 id            INTEGER NOT NULL primary key AUTOINCREMENT,"
			"                 cliaddr       INTEGER NOT NULL, "
			"                 total_reads   INTEGER NOT NULL, "
			"                 total_hits    INTEGER NOT NULL, "
			"                 UNIQUE(cliaddr) ON CONFLICT REPLACE)";

		char create_histgram_reads[] =
			"create table reads ("
			"                  id         INTEGER NOT NULL primary key AUTOINCREMENT,"
			"                  entry_id   INTEGER NOT NULL,"
			"                  client_id  INTEGER NOT NULL,"
			"                  inum       UINT8 NOT NULL,"
			"                  pagenum    UINT8 NOT NULL,"
			"                  count      INTEGER NOT NULL,"
			"                  UNIQUE(entry_id) ON CONFLICT REPLACE)";		

	create_table_1:
		rc = sqlite3_exec(db_p, create_clients, 0, 0, &errmsg);
		if (rc != SQLITE_OK) {
			if (second_chance) {
				rc = sqlite3_exec(db_p, "DROP TABLE clients", 0, 0, &errmsg);
				if (rc != SQLITE_OK) {
					gflog_fatal_errno(GFARM_MSG_1000599,
									  "Could not drop table %s",
									  errmsg); 
					sqlite3_free(errmsg);
				}
				second_chance = 0;
				goto create_table_1;
			}
			gflog_fatal_errno(GFARM_MSG_1000599,
				  "Could not create or recreate a table for recording connected clients: %s",
				  errmsg);
		}

		second_chance = 1;
		
	create_table_2:
		rc = sqlite3_exec(db_p, create_histgram_reads, 0, 0, &errmsg);
		if (rc != SQLITE_OK) {
			if (second_chance) {
				rc = sqlite3_exec(db_p, "DROP TABLE reads", 0, 0, &errmsg);
				if (rc != SQLITE_OK) {
					gflog_fatal_errno(GFARM_MSG_1000599,
									  "Could not drop table %s",
									  errmsg); 
					sqlite3_free(errmsg);					
				}
				second_chance = 0;
				goto create_table_2;
			}
			gflog_fatal_errno(GFARM_MSG_1000599,
				   "Could not create or recreate a table for recording read histgrams: %s",
				   errmsg);
		}
	}

	*db = db_p;
}

void gfp_free_histgram(sqlite3 *db)
{
	sqlite3_free(db);
}

