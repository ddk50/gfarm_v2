# $Id: Makefile 2375 2006-01-12 02:06:20Z soda $

top_builddir = ../../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

SUBDIRS=debian default redhat suse

datadir=$(default_datadir)/gfarm/config

DATA =	$(srcdir)/config-gfarm.sysdep

include $(top_srcdir)/makes/data.mk
include $(top_srcdir)/makes/subdir.mk
