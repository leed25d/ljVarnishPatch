# $Id: Makefile.am 3158 2008-09-03 07:25:27Z phk $

INCLUDES = -I$(top_srcdir)/include

bin_PROGRAMS = varnishadm

dist_man_MANS = varnishadm.1

varnishadm_SOURCES = \
	varnishadm.c

varnishadm_LDADD = \
	$(top_builddir)/lib/libvarnish/libvarnish.la \
	$(top_builddir)/lib/libvarnishcompat/libvarnishcompat.la \
	${PTHREAD_LIBS} ${NET_LIBS}
