
# common flags used in openocd build
AM_CPPFLAGS = -I$(top_srcdir)/src \
			  -I$(top_builddir)/src \
			  -I$(top_srcdir)/src/helper \
			  -DPKGDATADIR=\"$(pkgdatadir)\" \
			  -DPKGLIBDIR=\"$(pkglibdir)\"

if INTERNAL_JIMTCL
AM_CPPFLAGS += -I$(top_srcdir)/jimtcl \
			   -I$(top_builddir)/jimtcl
endif

if CMSIS_DAP
if INTERNAL_HIDAPI
AM_CPPFLAGS += -I$(top_srcdir)/hidapi
endif
endif
