# $Id: Makefile.am 4581 2010-02-21 20:36:31Z phk $

INCLUDES = -I$(top_srcdir)/include

lib_LTLIBRARIES = libvarnishcompat.la

libvarnishcompat_la_LDFLAGS = -version-info 1:0:0

libvarnishcompat_la_SOURCES = \
	daemon.c \
	execinfo.c \
	setproctitle.c \
	srandomdev.c \
	strlcat.c \
	strlcpy.c \
	strndup.c
