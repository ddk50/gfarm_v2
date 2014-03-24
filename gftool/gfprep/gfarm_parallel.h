/*
 * $Id: gfarm_parallel.h 8394 2013-07-25 10:42:55Z takuya-i $
 */

typedef struct gfpara gfpara_t;
typedef struct gfpara_proc gfpara_proc_t;

enum gfpara_status {
	GFPARA_NEXT,
	GFPARA_END,
	GFPARA_FATAL
};

enum gfpara_interrupt {
	GFPARA_INTR_RUN,
	GFPARA_INTR_TERM,
	GFPARA_INTR_STOP
};

gfarm_error_t gfpara_init(gfpara_t **, int,
			  int (*)(void *, FILE *, FILE *), void *,
			  int (*)(FILE *, gfpara_proc_t *, void *, int),
			  void *,
			  int (*)(FILE *, gfpara_proc_t *, void *), void *,
			  void *(*)(void *), void *);
gfarm_error_t gfpara_start(gfpara_t *);
gfarm_error_t gfpara_join(gfpara_t *);
gfarm_error_t gfpara_terminate(gfpara_t *, int);
gfarm_error_t gfpara_stop(gfpara_t *);

void gfpara_recv_int(FILE *, gfarm_int32_t *);
void gfpara_recv_int64(FILE *, gfarm_int64_t *);
void gfpara_recv_string(FILE *, char **);
void gfpara_recv_purge(FILE *);
void gfpara_send_int(FILE *t, gfarm_int32_t);
void gfpara_send_int64(FILE *, gfarm_int64_t);
void gfpara_send_string(FILE *, const char *, ...) GFLOG_PRINTF_ARG(2, 3);

pid_t gfpara_pid_get(gfpara_proc_t *);
void *gfpara_data_get(gfpara_proc_t *);
void gfpara_data_set(gfpara_proc_t *, void *);
gfpara_proc_t *gfpara_procs_get(gfpara_t *);
