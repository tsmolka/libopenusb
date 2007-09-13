
INCLUDES = -I$(top_srcdir)/src

noinst_PROGRAMS = testopenusb

testopenusb_SOURCES = testopenusb.c
testopenusb_LDADD = $(top_builddir)/src/libusb.la @OSLIBS@ -lusb

#testopenusb_la_LDFLAGS = -lusb
