#!/bin/sh

. ./regress.conf

if gfgroup does-not-exist; then
	:
else
	exit_code=$exit_pass
fi

exit $exit_code
