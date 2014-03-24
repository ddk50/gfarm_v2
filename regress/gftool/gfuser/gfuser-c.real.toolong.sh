#!/bin/sh

. ./regress.conf

user=tmpusr-`hostname`-$$
# 257 characters (GFARM_USER_REALNAME_MAX in gfm_proto.h)
real=XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
home=/home/$user
gsi_dn=''

if $regress/bin/am_I_gfarmadm; then
    :
else
    exit $exit_unsupported
fi

trap 'gfuser -d "$user" 2>/dev/null; rm -f $localtmp; exit $exit_trap' $trap_sigs

if gfuser -c "$user" "$real" "$home" "$gsi_dn" 2>$localtmp; then
    gfuser -d "$user"
else
    if grep ': invalid argument$' $localtmp >/dev/null; then
	exit_code=$exit_pass
    fi
fi

rm -f $localtmp
exit $exit_code
