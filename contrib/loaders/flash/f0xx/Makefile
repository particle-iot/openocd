ifneq ($(MAKECMDGOALS), clean)
ifeq ($(FAPI_PATH),)
$(error FAPI_PATH is not defined. See README for more information.)
endif
endif

MCU    ?= tms570
DEVICE  = $(shell echo $(MCU) | tr a-z A-Z)
TARGET  = $(MCU)-helper

SRCS    = main.c printf.c
OBJS    = $(SRCS:.c=.o)
PREFIX ?= arm-none-eabi-

OBJCOPY = $(PREFIX)objcopy
CC      = $(PREFIX)gcc
OBJDUMP = $(PREFIX)objdump

RM = rm
CP = cp

CFLAGS  = -Wall -std=gnu99 -O0 -flto -fshort-wchar -g -D$(DEVICE)
CPU_FLAGS = -mthumb -march=armv7-r -mtune=cortex-r4 -mbig-endian -nostdlib
CFLAGS += $(CPU_FLAGS)

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG
endif

LDFLAGS =  $(CPU_FLAGS) -g -gdwarf-2 \
	   -T$(MCU).ld -Tsections.ld \
	   -Wl,-Map=$(TARGET).map


INCDIR = -I${FAPI_PATH}/include -I../../../../src -I../../../../src/helper  -I../../../../jimtcl

all:	 compiled/$(TARGET).h $(TARGET).lss

compiled/$(TARGET).h: $(TARGET).elf
	python elf-to-h.py $< > $@

$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCDIR) -c $<

$(TARGET).elf: $(OBJS) ${FAPI_PATH}/F021_API_CortexR4_BE.lib
	$(Q)$(CC) $^ $(LDFLAGS) -o $@

$(TARGET).lss: $(TARGET).elf
# Objdump is will segfault for generated ELF file so comment it out for now
# $(OBJDUMP) -h -S $< > $@

.PHONY: clean
clean:
	$(RM) -rf $(OBJS) *.elf *.lss compiled/$(TARGET).h
