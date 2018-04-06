.PHONY: gperf_phony

SOURCES = \
	freertos_gperf.c \
	rtos_gperf.c

GPERF_OPTIONS = -L ANSI-C -t

all: gperf
gperf: $(SOURCES)
gperf_phony:

%_gperf.c: %.gperf gperf_phony
	gperf $(GPERF_OPTIONS) $< > $@
	./gperf_convert.py $< $@ $(basename $@).h
