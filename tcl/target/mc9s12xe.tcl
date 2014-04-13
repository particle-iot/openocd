if { [info exists CHIPNAME] } {
   set _CHIPNAME $CHIPNAME
} else {
   set _CHIPNAME mc9s12xe
}

if { [info exists ENDIAN] } {
   set _ENDIAN $ENDIAN
} else {
   set _ENDIAN big
}

interface usbdm

bdm newdap $_CHIPNAME cpu -irlen 4 -ircapture 0x1 -irmask 0xf -expected-id 0xBEEF

set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME hcs12 -endian $_ENDIAN -chain-position $_TARGETNAME

$_TARGETNAME configure -work-area-phys 0x000FE000 -work-area-size 0x2000 -work-area-backup 0

# flash size will be probed
set _FLASHNAME $_CHIPNAME.flash
flash bank $_FLASHNAME s12xftm 0x7FFFFF 0 0 0 $_TARGETNAME
