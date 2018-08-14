BIN2C = ../../../../src/helper/bin2char.sh
CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy

CFLAGS = -march=armv7-r -marm -Og -c

all: ec1_loader.inc

%.elf: %.c
	$(CC) $(CFLAGS) $< -o $@

%.bin: %.elf
	$(OBJCOPY) -Obinary $< $@

%.inc: %.bin
	$(BIN2C) < $< > $@

clean:
	-rm -f *.elf *.bin *.inc
