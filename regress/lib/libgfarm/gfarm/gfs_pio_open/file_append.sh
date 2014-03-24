#!/bin/sh

. ./regress.conf

gfs_pio_test=`dirname $testbin`/gfs_pio_test/gfs_pio_test

trap 'gfrm -f $gftmp; rm -f $localtmp; exit $exit_trap' $trap_sigs

if gfreg $data/1byte $gftmp &&
   echo abcde | $gfs_pio_test -a -W 5 $* $gftmp >$localtmp &&
   [ `LC_ALL=C wc -c <$localtmp` = 0 ] &&
   gfls -l $gftmp |
	awk '{size=$5; print size; if (size == 6) exit(0); else exit(1) }'
then
    exit_code=$exit_pass
fi

gfrm $gftmp
rm -f $localtmp
exit $exit_code
