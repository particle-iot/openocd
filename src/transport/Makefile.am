include $(top_srcdir)/common.mk

noinst_LTLIBRARIES = liboocdtransport.la
liboocdtransport_la_SOURCES = \
	transport.c \
	swd.c \
	swd_tcl.c \
	swd_libswd.c

noinst_HEADERS = \
	transport.h \
	swd.h \
	swd_libswd.h

MAINTAINERCLEANFILES = $(srcdir)/Makefile.in
