#!/bin/sh
# $Id: mkfiles.sh 6389 2012-07-02 05:12:52Z devtty $

TARGET=./.files
rm -rf ${TARGET}
echo 'INPUT = \' >> ${TARGET}
find . -type f -name '*.c' -o -name '*.cpp' -o -name '*.h' | \
    awk '{ printf "%s \\\n", $1 }' | \
    grep -v 'check/' >> ${TARGET}
echo >> ${TARGET}
