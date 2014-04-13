MCU   ?= mc9s12xe
TARGET = ocd-helper

SRCS   = main.c
OBJS = $(SRCS:.c=.o)

PREFIX ?= m68hc11-elf-

OBJCOPY = $(PREFIX)objcopy
CC      = $(PREFIX)gcc
OBJDUMP = $(PREFIX)objdump
RM = rm
CP = cp

CFLAGS  = -m68hc12 -O2 -fno-ident -fno-common -fomit-frame-pointer -mshort -nostdlib
LDFLAGS = -T$(MCU)-memory.ld -T$(MCU).ld
OBJCOPYFLAGS =
INCDIR = -I./include -I../../../../src -I../../../../src/helper  -I../../../../jimtcl

all:	$(TARGET).elf $(TARGET).bin $(TARGET).lss

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $<

$(TARGET).elf: $(OBJS) $(MCU).ld $(MCU)-memory.ld
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).lss: $(TARGET).elf
	$(OBJDUMP) -h -S $< > $@

.PHONY: clean
clean:
	$(RM) -rf $(OBJS) *.elf *.bin *.lss;
