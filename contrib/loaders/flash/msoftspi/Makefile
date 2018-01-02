BIN2C = ../../../../src/helper/bin2char.sh

SRCS=msoftspi_read.S msoftspi_write.S msoftspi_erase_check.S \
	msoftdpi_read.S msoftdpi_write.S msoftdpi_erase_check.S \
	msoftqpi_read.S msoftqpi_write.S msoftqpi_erase_check.S \
	msofti2c_read.S msofti2c_write.S msofti2c_erase_check.S \
	msofti2c_calib.S msofti2c_scan.S

OBJS=$(patsubst %.S,%.inc,$(SRCS))

CROSS_COMPILE ?= arm-none-eabi-

CC=$(CROSS_COMPILE)gcc
OBJCOPY=$(CROSS_COMPILE)objcopy
OBJDUMP=$(CROSS_COMPILE)objdump
LD=$(CROSS_COMPILE)ld

all: $(OBJS)

%.o: %.S msoftspi.s ../../../../src/flash/nor/msoftspi.h
	$(CC) -Wa,-adlmn -o $@ -c $< > $(@:.o=.lst)
	@enscript -Easm -T 4 -G -o - $(@:.o=.lst) | ps2pdf - $(@:.o=.pdf) || true

%.elf: %.o
	$(LD) -s -defsym=_start=0 -o $@ $<

%.bin: %.elf
	$(OBJCOPY) -S -O binary $< $@

%.inc: %.bin
	$(BIN2C) < $< > $@

clean:
	-rm -f $(patsubst %.S,%.o,$(SRCS)) $(patsubst %.S,%.elf,$(SRCS)) $(patsubst %.S,%.bin,$(SRCS))
	-rm -f $(patsubst %.S,%.inc,$(SRCS)) $(patsubst %.S,%.pdf,$(SRCS)) $(patsubst %.S,%.lst,$(SRCS))

.PHONY:	all clean

.INTERMEDIATE: $(patsubst %.S,%.o,$(SRCS)) $(patsubst %.S,%.elf,$(SRCS)) $(patsubst %.S,%.bin,$(SRCS))
