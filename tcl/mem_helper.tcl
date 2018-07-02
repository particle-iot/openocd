# Helper for common memory read/modify/write procedures

# helpers for mrw
proc mrw_virt {reg} {
	set value ""
	mem2array value 32 $reg 1
	return $value(0)
}

proc mrw_phys {reg} {
	set value ""
	mem2array value 32 $reg 1 phys
	return $value(0)
}

# mrw: "memory read word", returns value of word in memory
proc mrw {a {b ""}} {
	if {$b eq ""} {
		return [mrw_virt $a];
	}
	if {$a eq "phys"} {
		return [mrw_phys $b];
	}
	return -code error "wrong args: should be \" mrw \[phys\] address\""
}

add_usage_text mrw "\[phys\] address"
add_help_text mrw "Returns value of word in memory."

proc mrb {reg} {
	set value ""
	mem2array value 8 $reg 1
	return $value(0)
}

add_usage_text mrb "address"
add_help_text mrb "Returns value of byte in memory."

# helpers for mmw
proc mmw_virt {reg setbits clearbits} {
	set old [mrw_virt $reg]
	set new [expr ($old & ~$clearbits) | $setbits]
	mww $reg $new
}

proc mmw_phys {reg setbits clearbits} {
	set old [mrw_phys $reg]
	set new [expr ($old & ~$clearbits) | $setbits]
	mww phys $reg $new
}

# mmw: "memory modify word", updates value of word in memory
#       $reg <== ((value & ~$clearbits) | $setbits)
proc mmw {a b c {d ""}} {
	if {$d eq ""} {
		mmw_virt $a $b $c
		return "";
	}
	if {$a eq "phys"} {
		return [mmw_phys $b $c $d]
	}
	return -code error "wrong args: should be \" mmw \[phys\] address setbits clearbits\""
}

add_usage_text mmw "\[phys\] address setbits clearbits"
add_help_text mmw "Modify word in memory. new_val = (old_val & ~clearbits) | setbits;"
