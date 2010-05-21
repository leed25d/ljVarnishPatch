# $Id: Makefile.am 3140 2008-08-29 09:37:19Z tfheen $

INCLUDES = -I$(top_srcdir)/include

lib_LTLIBRARIES = libvarnishapi.la

libvarnishapi_la_LDFLAGS = -version-info 1:0:0

libvarnishapi_la_SOURCES = \
	base64.c \
	instance.c \
	shmlog.c

libvarnishapi_la_CFLAGS = \
	-DVARNISH_STATE_DIR='"${VARNISH_STATE_DIR}"'
