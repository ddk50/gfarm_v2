/*
 * $Id: nata_rcsid.h 6389 2012-07-02 05:12:52Z devtty $
 */
#ifndef __NATA_RCSID_H__
#define __NATA_RCSID_H__

#ifdef __GNUC__
#define USE_BOODOO	__attribute__((used))
#else
#define USE_BOODOO	/**/
#endif /* __GNUC__ */

#define __rcsId(id) \
static volatile const char USE_BOODOO *rcsid(void) { \
    return id ? id : rcsid(); \
}

#endif /* __NATA_RCSID_H__ */

