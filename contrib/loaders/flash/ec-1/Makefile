BIN2C = xxd
CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy

CFLAGS = -march=armv7-r -marm -Og -c

all: ec1_loader.inc

%.elf: %.c
	$(CC) $(CFLAGS) $< -o $@

%.bin: %.elf
	$(OBJCOPY) -Obinary $< code

%.inc: %.bin
	$(BIN2C) -c11 -i code code.inc
	rm code

clean:
	-rm -f *.elf code *.inc
