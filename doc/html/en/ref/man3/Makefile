# $Id: Makefile 8426 2013-07-29 01:05:54Z m-kasahr $

top_builddir = ../../../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

htmldir=$(default_htmldir)/en/ref/man3

DOCBOOK_DIR=$(top_srcdir)/doc/docbook/en/ref/man3
include $(DOCBOOK_DIR)/Makefile.inc
HTMLSRC=$(DOCBOOK)
DOCBOOK2HTML_XSL=../docbook2html.xsl

include $(top_srcdir)/makes/html.mk
