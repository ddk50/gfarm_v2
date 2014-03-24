#!/bin/sh
# $Id: doDosText.sh 6389 2012-07-02 05:12:52Z devtty $

l=`find . -type f \( \
    -name '*.h' -o \
    -name '*.c' -o \
    -name '*.cpp' -o \
    -name 'Makefile.in' \)`

for i in ${l}; do
    nkf -c ${i} > ${i}.tmp
    rm -f ${i}
    mv -f ${i}.tmp ${i}
done

exit 0


