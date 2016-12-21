SRCS=stmqspi_erase_check.S stmqspi_write.S stmoctospi_write.S
OBJS=$(patsubst %.S,%.c,$(SRCS))
CODE=../../../../src/flash/nor/stmqspi.c
VPATH=../../erase_check

CROSS_COMPILE ?= arm-none-eabi-

CC=$(CROSS_COMPILE)gcc
OBJCOPY=$(CROSS_COMPILE)objcopy
OBJDUMP=$(CROSS_COMPILE)objdump
LD=$(CROSS_COMPILE)ld

all: $(OBJS)

%.o: %.S Makefile
	$(CC) -Wa,-adlmn -o $@ -c $< > $(@:.o=.lst)
	@enscript -Easm -T 4 -G -o - $(@:.o=.lst) | ps2pdf - $(@:.o=.pdf) || true

%.x: %.o
	$(LD) -s -defsym=_start=0 -o $@ $<

%.bin: %.x
	$(OBJCOPY) -S -O binary $< $@

%.c: %.bin
	@echo -e "\tstatic const uint8_t $(@:.c=)_code[] = {" > $@
	@cat "$<" | xxd -i | sed -r 's/^\s*/\t\t/' >> $@
	@echo -e "\t};" >> $@
	sed -i -e "/$(patsubst %.c,%,$@).*code\\[\\]/,/};/d" "$(CODE)" \
		-e "/see\\s\+contrib.*$(patsubst %.c,%,$@)/r$@"

clean:
	rm -f *.c *.lst *.pdf

.SUFFIXES:

.PHONY:	all clean
