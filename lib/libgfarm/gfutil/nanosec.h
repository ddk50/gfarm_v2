/*
 * $Id: nanosec.h 7175 2012-11-23 18:07:17Z takuya-i $
 */

#define GFARM_SECOND_BY_NANOSEC   1000000000
#define GFARM_MILLISEC_BY_NANOSEC 1000000
#define GFARM_MICROSEC_BY_NANOSEC 1000

void gfarm_nanosleep_by_timespec(const struct timespec *);
void gfarm_nanosleep(unsigned long long);

void gfarm_gettime(struct timespec *);

int gfarm_local_lutimens(const char *, const struct timespec [2]);
int gfarm_local_utimens(const char *, const struct timespec [2]);
