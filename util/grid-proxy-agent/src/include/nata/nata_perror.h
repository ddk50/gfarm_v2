/* 
 * $Id: nata_perror.h 6389 2012-07-02 05:12:52Z devtty $
 */
#include <nata/nata_logger.h>

#ifdef perror
#undef perror
#endif /* perror */
#define perror(str)	nata_MsgError("%s: %s\n", str, strerror(errno))
