
TOPDIR =      ../../../..
BIN2C = $(TOPDIR)/src/helper/bin2char.sh

CROSS_COMPILE   ?=      arm-none-eabi-

CC	= $(CROSS_COMPILE)gcc
LINKER  = $(CROSS_COMPILE)gcc
OBJCOPY	= $(CROSS_COMPILE)objcopy
OBJDUMP	= $(CROSS_COMPILE)objdump

CC_OPTS	:= -mcpu=cortex-m3 -mthumb -mlittle-endian -O0
CC_OPTS	+= -fmessage-length=0 -fsigned-char -std=gnu11
CC_OPTS	+= -ffunction-sections -fdata-sections
CC_OPTS	+= -DOCD -DPRTCC

LINKER_SCRIPT	:= etacorem3.ld
LINKER_OPTS	:= --specs=nosys.specs -mcpu=cortex-m3 -mthumb
LINKER_OPTS     += -mlittle-endian -O0 -fmessage-length=0 -fsigned-char
LINKER_OPTS     += -ffunction-sections -fdata-sections -Wl,--gc-sections
LINKER_OPTS     += -nostartfiles -nostdlib
LINKER_OPTS     +=  -Xlinker -Map=$@.map

.INTERMEDIATE: erase.elf write.elf

all: erase.inc write.inc

erase.elf write.elf: etacorem3_flash_common.h

%.bin:	%.elf
	$(OBJCOPY) --verbose -O binary "$<"  "$@"

%.elf : %.s
	$(LINKER) $(LINKER_OPTS) -T $(LINKER_SCRIPT) -o "$@" "$<"

%.txt:	%.elf
	$(CROSS_COMPILE)objdump -S -d "$<" > $@

%.inc: %.bin
	$(BIN2C) < $< > $@

%.s: %.c
	$(CC) $(CC_OPTS) $(EXTRA_CFLAGS) -S -Wa,-a,-ad -o "$@" "$<"

clean:
	rm -f *.d *.bin *.elf *.inc *.s *.map *.lst
