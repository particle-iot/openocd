BIN2C = ../../../../src/helper/bin2char.sh

CROSS_COMPILE ?= arm-none-eabi-

CC=$(CROSS_COMPILE)gcc
OBJCOPY=$(CROSS_COMPILE)objcopy
OBJDUMP=$(CROSS_COMPILE)objdump

LINKER_SCRIPT := etacorem3.ld
LINKER_OPTS := -mcpu=cortex-m3 -mthumb
LINKER_OPTS += -mlittle-endian -O0 -fmessage-length=0 -fsigned-char
LINKER_OPTS += -ffunction-sections -fdata-sections -gc-sections
LINKER_OPTS += -nostartfiles -nostdlib --specs=nosys.specs

all: erase.inc write.inc

.PHONY: clean

.INTERMEDIATE: erase.elf write.elf

erase.elf write.elf:

%.elf: %.s
	$(CC) $(LINKER_OPTS) -T $(LINKER_SCRIPT) "$<" -o "$@"

%.lst: %.elf
	$(OBJDUMP) -S -d $< > $@

%.bin: %.elf
	$(OBJCOPY) -Obinary $< $@

%.inc: %.bin
	$(BIN2C) < $< > $@

clean:
	-rm -f *.elf *.lst *.bin *.inc
