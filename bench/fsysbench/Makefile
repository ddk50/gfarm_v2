# $Id: Makefile 2533 2006-03-31 10:50:56Z takuya $

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDFLAGS) $(LIBS)

PROGRAM = fsysbench
OBJS = fsysbench.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
