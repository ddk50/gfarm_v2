/* 
 * $Id: ScopedLock.h 6389 2012-07-02 05:12:52Z devtty $
 */
#ifndef __SCOPEDLOCK_H__
#define __SCOPEDLOCK_H__

#include <nata/nata_rcsid.h>

#include <nata/nata_includes.h>

#include <nata/nata_macros.h>

#include <nata/Mutex.h>

#include <nata/nata_perror.h>





class ScopedLock {



private:
    __rcsId("$Id: ScopedLock.h 6389 2012-07-02 05:12:52Z devtty $");

    Mutex *mMtxPtr;

    ScopedLock(const ScopedLock &obj);
    ScopedLock operator = (const ScopedLock &obj);



public:
    ScopedLock(Mutex *mPtr) :
        mMtxPtr(mPtr) {
        if (mPtr != NULL) {
            mPtr->lock();
        } else {
            fatal("invalid mutex.\n");
        }
    }

    ~ScopedLock(void) {
        mMtxPtr->unlock();
    }

};


#endif // ! __SCOPEDLOCK_H__
