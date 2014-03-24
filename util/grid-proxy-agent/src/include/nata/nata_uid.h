/* 
 * $Id: nata_uid.h 6389 2012-07-02 05:12:52Z devtty $
 */
#ifndef __NATA_UID_H__
#define __NATA_UID_H__


typedef uint8_t nata_Uid[32];

#if defined(__cplusplus)
extern "C" {
#endif /* __cplusplus */

extern void		nata_initUid(void);
extern void		nata_getUid(nata_Uid *uPtr);

#if defined(__cplusplus)
}
#endif /* __cplusplus */


#endif /* ! __NATA_UID_H__ */
