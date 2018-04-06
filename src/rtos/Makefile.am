noinst_LTLIBRARIES += %D%/librtos.la
%C%_librtos_la_SOURCES = \
	%D%/rtos.c \
	%D%/rtos_standard_stackings.c \
	%D%/rtos_ecos_stackings.c  \
	%D%/rtos_chibios_stackings.c \
	%D%/rtos_embkernel_stackings.c \
	%D%/rtos_mqx_stackings.c \
	%D%/rtos_ucos_iii_stackings.c \
	%D%/FreeRTOS.c \
	%D%/ThreadX.c \
	%D%/eCos.c \
	%D%/linux.c \
	%D%/ChibiOS.c \
	%D%/embKernel.c \
	%D%/mqx.c \
	%D%/uCOS-III.c \
	%D%/rtos.h \
	%D%/rtos_standard_stackings.h \
	%D%/rtos_ecos_stackings.h \
	%D%/linux_header.h \
	%D%/rtos_chibios_stackings.h \
	%D%/rtos_embkernel_stackings.h \
	%D%/rtos_mqx_stackings.h \
	%D%/rtos_ucos_iii_stackings.h

%C%_librtos_la_CFLAGS = $(AM_CFLAGS)

noinst_LTLIBRARIES += %D%/librtos_gperf.la
%C%_librtos_gperf_la_SOURCES = \
	%D%/freertos_gperf.c \
	%D%/freertos_gperf.h \
	%D%/rtos_gperf.c \
	%D%/rtos_gperf.h

%C%_librtos_gperf_la_CFLAGS = $(AM_CFLAGS) -Wno-missing-field-initializers

if HAVE_GPERF_VERSION
if HAVE_PYTHON3

GPERF_OPTIONS = -L ANSI-C -t

BUILT_SOURCES += \
	%D%/freertos_gperf.c \
	%D%/freertos_gperf.h \
	%D%/rtos_gperf.c \
	%D%/rtos_gperf.h

%_gperf.c %_gperf.h: %.gperf
	gperf $(GPERF_OPTIONS) $< > $@
	$(srcdir)/%D%/gperf_convert.py $< $@ $(basename $@).h

CLEANFILES += \
	$(BUILT_SOURCES)

endif # HAVE_PYTHON3
endif # HAVE_GPERF_VERSION

if IS_MINGW
# FD_* macros are sloppy with their signs on MinGW32 platform
%C%_librtos_la_CFLAGS += -Wno-sign-compare
endif
