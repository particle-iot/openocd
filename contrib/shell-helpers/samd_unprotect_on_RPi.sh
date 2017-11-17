# Shell script placing a SAMD device into cold plugging mode and mass-erasing its flash from a Raspberry Pi.
# This is necessary to be able to mass-erase a secured chip.
# Notice:	This script needs root rights to have access to the RPi's GPIOs.
# Usage:	sudo sh samd_unprotect_on_RPi.sh
# Author:	Tobias Ferner, 2017
# 		tobias.ferner@gmail.com

# Set proper swd pins here (bcm2835 numbering):
SWCLK=27
SWDIO=17
RST=22

echo "Exporting SWCLK and RST pins."
echo $SWCLK > /sys/class/gpio/export
echo $RST > /sys/class/gpio/export
echo "out" > /sys/class/gpio/gpio$SWCLK/direction
echo "out" > /sys/class/gpio/gpio$RST/direction

echo "Setting SWCLK low and pulsing RST."
echo "0" > /sys/class/gpio/gpio$SWCLK/value
echo "0" > /sys/class/gpio/gpio$RST/value
echo "1" > /sys/class/gpio/gpio$RST/value

echo "Unexporting SWCLK and RST pins."
echo $SWCLK > /sys/class/gpio/unexport
echo $RST > /sys/class/gpio/unexport

echo "Ready for mass erase."
openocd -c "source [find interface/raspberrypi2-native.cfg]; bcm2835gpio_swd_nums $SWCLK $SWDIO; bcm2835gpio_srst_num $RST; transport select swd; set CHIPNAME samd20; source [find target/at91samdXX.cfg]; adapter_khz 276; init; reset; reset; at91samd chip-erase; shutdown"
echo "Done."
