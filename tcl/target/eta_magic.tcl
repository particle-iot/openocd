#
# Eta Compute ECM35xx
#
# http://www.etacompute.com
# Rick Foos <rfoos@solengtech.com>
#
# Use setmagic command prior to running SRAM program.
# Use clearmagic to run standard bootrom programs after reset.

# This puts SWD in a slow mode to read/write without WAIT.
proc swdslow { } {
    ecm35xx.dap memaccess 400
    adapter_khz 1000
    echo "SWD in slow mode."
}
# This puts SWD in normal mode.
proc swdnormal { } {
    ecm35xx.dap memaccess 8
    echo "SWD in normal mode."
}

proc setmagic  { } {
    mww 0x1001FFF0 0xc001c0de
    mww 0x1001FFF4 0xc001c0de
    mww 0x1001FFF8 0xdeadbeef
    mww 0x1001FFFC 0xc369a517
    echo "Magic Bits Set"
}

proc showmagic { } {
	echo "Show Magic Bits"
	mdw 0x1001FFF0  4
}

proc clearmagic { } {
    mww 0x1001FFF0 0 4
    echo "Magic Bits Cleared"
}
