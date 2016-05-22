.PHONY: arm clean-arm

all: arm

common_dirs = \
	checksum \
	erase_check \
	flash/kinetis_ke \
	watchdog

ARM_CROSS_COMPILE ?= arm-none-eabi-

arm_dirs = \
	flash/fm4 \
	flash/xmc1xxx

arm:
	#$(MAKE) -C flash/at91sam7x all TRGT=$(ARM_CROSS_COMPILE)
	for d in $(common_dirs); do \
		$(MAKE) -C $$d arm; \
	done
	for d in $(arm_dirs); do \
		$(MAKE) -C $$d all CROSS_COMPILE=$(ARM_CROSS_COMPILE); \
	done

clean-arm:
	for d in $(arm_dirs) flash/at91sam7x; do \
		$(MAKE) -C $$d clean; \
	done

clean: clean-arm
	for d in checksum erase_check watchdog; do \
		$(MAKE) -C $$d clean; \
	done
