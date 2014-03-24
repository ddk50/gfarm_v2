/*
 * $Id: gfprep.h 6070 2012-03-13 09:59:58Z takuya-i $
 */

extern const char GFPREP_FILE_URL_PREFIX[];
#define GFPREP_FILE_URL_PREFIX_LENGTH 5 /* file: */

int gfprep_url_is_local(const char *);
int gfprep_url_is_gfarm(const char *);
int gfprep_vasprintf(char **, const char *, va_list) GFLOG_PRINTF_ARG(2, 0);
int gfprep_asprintf(char **, const char *, ...) GFLOG_PRINTF_ARG(2, 3);
