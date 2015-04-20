CROSS_COMPILE ?= arm-none-eabi-

CC=$(CROSS_COMPILE)gcc
OBJCOPY=$(CROSS_COMPILE)objcopy
OBJDUMP=$(CROSS_COMPILE)objdump

all: erase.inc write.inc

%.elf: %.S
	$(CC) -static -nostartfiles $< -o $@

%.lst: %.elf
	$(OBJDUMP) -S $< > $@

%.bin: %.elf
	$(OBJCOPY) -Obinary $< $@

%.inc: %.bin
	xxd -i < $< > $@

clean:
	-rm -f *.elf *.lst *.bin *.inc
