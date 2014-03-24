/*
 * $Id: bool.h 6182 2012-03-28 08:21:23Z devtty $
 */
#if !defined(__cplusplus)
#if __STDC_VERSION__ >= 199901L
#include <stdbool.h>
#else
typedef enum {
	false = 0,
	true = 1
} bool;
#endif /* __STDC_VERSION__ >= 199901L */
#endif /* ! __cplusplus */
