# Build arm object, convert to inc file for OpenOCD include.
# For target debuging, make xxx.elf.
TOPDIR = ../../../..
BIN2C = $(TOPDIR)/src/helper/bin2char.sh
PROJECT_NAME := $(notdir $(CURDIR))

CROSS_COMPILE   ?=      arm-none-eabi-

CC	= $(CROSS_COMPILE)gcc
LINKER  = $(CROSS_COMPILE)gcc
OBJCOPY	= $(CROSS_COMPILE)objcopy
OBJDUMP	= $(CROSS_COMPILE)objdump

CC_OPTS	:= -mcpu=cortex-m3 -mthumb -mlittle-endian -O0
CC_OPTS	+= -fmessage-length=0 -fsigned-char -std=gnu11
CC_OPTS	+= -ffunction-sections -fdata-sections -g3 -Werror
CC_OPTS	+= -DOCD -DPRTCC

LINKER_SCRIPT	:= etacorem3.ld
LINKER_OPTS	= --specs=nosys.specs -mcpu=cortex-m3 -mthumb
LINKER_OPTS     += -mlittle-endian -O0 -fmessage-length=0 -fsigned-char
LINKER_OPTS     += -ffunction-sections -fdata-sections -Wl,--gc-sections
LINKER_OPTS     += -nostartfiles -nostdlib -static
LINKER_OPTS     +=  -Xlinker -Map="$(basename $@).map" -g3

# C source > s source > incs > elves
SRC := $(wildcard *.c)
ELVES := $(SRC:.c=.elf)
EINCS := $(SRC:.c=.inc)

.INTERMEDIATE: $(ELVES)

all: $(EINCS)

$(ELVES) : etacorem3_flash_common.h

%.bin:	%.elf
	$(OBJCOPY) --verbose -O binary "$<"  "$@"

%.elf : %.s
	$(LINKER) $(LINKER_OPTS) -T $(LINKER_SCRIPT) -o "$@" "$<"

%.txt:	%.elf
	$(CROSS_COMPILE)objdump -S -d "$<" > "$@"

%.inc: %.bin
	$(BIN2C) < "$<" > "$@"

%.s: %.c
	$(CC) $(CC_OPTS) $(EXTRA_CFLAGS) -S -Wa,-a,-ad -o "$@" "$<"

clean maintainer-clean:
	rm -f $(SRC:.c=.d) $(SRC:.c=.s) $(SRC:.c=.map) $(SRC:.c=.lst) $(SRC:.c=.bin) \
		$(ELVES) $(EINCS)
