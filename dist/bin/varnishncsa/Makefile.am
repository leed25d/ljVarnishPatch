# $Id: Makefile.am 3172 2008-09-08 16:53:30Z phk $

INCLUDES = -I$(top_srcdir)/include

bin_PROGRAMS = varnishncsa

dist_man_MANS = varnishncsa.1

varnishncsa_SOURCES = varnishncsa.c

varnishncsa_LDADD = \
	$(top_builddir)/lib/libvarnish/libvarnish.la \
	$(top_builddir)/lib/libvarnishcompat/libvarnishcompat.la \
	$(top_builddir)/lib/libvarnishapi/libvarnishapi.la \
	${PTHREAD_LIBS}
