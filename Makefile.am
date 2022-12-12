lib_LTLIBRARIES = libmedia.la

libmedia_la_SOURCES = media.c libplugin.h

libmedia_la_LDFLAGS = -avoid-version
libmedia_la_LIBADD = @LIBS@ @PILOT_LIBS@
libdir = $(prefix)/lib/jpilot/plugins

AM_CFLAGS = -Wall @PILOT_FLAGS@

local_install: libmedia.la
    ACLOCAL_AMFLAGS = -I m4
	$(INSTALL) -d -m 755 $(HOME)/.jpilot/plugins
	$(LIBTOOL) --mode=install $(INSTALL) -m 755 libmedia.la $(HOME)/.jpilot/plugins
