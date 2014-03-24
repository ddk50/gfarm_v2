# $Id: Makefile 6691 2012-09-28 11:29:12Z tatebe $

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

PROGRAM = gfstat
SRCS = $(PROGRAM).c
OBJS = $(PROGRAM).o
CFLAGS = $(COMMON_CFLAGS) -I$(GFARMLIB_SRCDIR) $(openssl_includes)
LDLIBS = $(COMMON_LDLIBS) $(GFARMLIB) $(LIBS)
DEPLIBS = $(DEPGFARMLIB)

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk

###

$(OBJS): $(DEPGFARMINC) \
	$(GFARMLIB_SRCDIR)/gfm_client.h \
	$(GFARMLIB_SRCDIR)/lookup.h \
	$(GFARMLIB_SRCDIR)/gfarm_path.h
