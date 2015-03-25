# This file contains script for mapping info or main memory block into memory map

    set userflashbase 0xA0022000
    set fmareg 0x00
    set fmdreg 0x04
    set fmcreg 0x08
    set fcisreg 0x0c
    set fcicreg 0x14

# Read status reg
proc getFCIS {} {
    set value ""
    mem2array value 32 0xA002200C 1
    return $value(0)
}

# Clear Status reg
proc clearFCIS {} {
    set value ""
    mww 0xA0022014 0x3
}

# Read a word from Info Block of USER Flash Memory
proc readFlashWord {addr} {
    global userflashbase 0xA0022000
    global fmareg 0x00
    global fmdreg 0x04
    global fmcreg 0x08
    global fcisreg 0x0c
    global fcicreg 0x14

    set value ""
    mwh [expr $userflashbase + $fmareg] $addr
    mww [expr $userflashbase + $fmcreg] 0xA4420040

    set waitTime 0
    while {[getFCIS] == 0} {
        set waitTime [expr $waitTime + 1]
        if {$waitTime > 20} {
            echo "Error while trying to map memory"
            return
        }
    }

    if {[getFCIS] == 0x3} {
        echo "Error while trying to map memory"
        return
    }

    clearFCIS

    mem2array value 8 [expr $userflashbase + $fmdreg] 1

    return $value(0)
}

# Write a word to Info Block of USER Flash Memory
proc writeFlashWord {addr word} {
    global userflashbase 0xA0022000
    global fmareg 0x00
    global fmdreg 0x04
    global fmcreg 0x08
    global fcisreg 0x0c
    global fcicreg 0x14

    set value ""
    mwh [expr $userflashbase + $fmareg] $addr
    mwh [expr $userflashbase + $fmdreg] $word
    mww [expr $userflashbase + $fmcreg] 0xA4420010

    set waitTime 0
    while {[getFCIS] == 0} {
        set waitTime [expr $waitTime + 1]
        if {$waitTime > 20} {
            echo "Error while trying to map memory TIMEOUT"
            return
        }
    }

    if {[getFCIS] == 0x3} {
        echo "Error while trying to map memory HARD"
        return
    }

    clearFCIS

    mem2array value 8 [expr $userflashbase + $fmdreg] 1

    return $value(0)
}

# Clear flash memory page
proc clearPage {page} {
    global userflashbase 0xA0022000
    global fmareg 0x00
    global fmdreg 0x04
    global fmcreg 0x08
    global fcisreg 0x0c
    global fcicreg 0x14

    clearFCIS
    set value ""
    mwh [expr $userflashbase + $fmareg] [expr $page << 8]
    mww [expr $userflashbase + $fmcreg] 0xA4420020

    set waitTime 0
    while {[getFCIS] == 0} {
        set waitTime [expr $waitTime + 1]
        if {$waitTime > 1000} {
            echo "Error while trying to erase page TIMEOUT"
            return
        }
    }

    if {[getFCIS] == 0x3} {
        echo "Error while trying to erase page HARD"
        return
    }

    clearFCIS
}

# Set flash memory type. If type == true - info block, else - main.
# To do this, we have to check and change (if needed) bit [0] in SystemSet register (let's call it SystemSet register =) ).
# This bit indicates memory block that is now mapped to 0x0 ... 0x2000. If 1 - main memory is mapped, if 0 - info.
# There's a small problem - this register is not memory mapped, so we have to read it via other regs.
# So we put SystemSet reg address in one register, "READ" command in another one, then get it in third Data register.
# And another problem (and it's a huge one) - this SystemSet register resides in NOR-flash memory, so we can't just turn "0" into "1".
# Need to erase. And only a whole page can be erased.

# The whole algorithm is:
#  1) Check bit 0 of SystemSet register
#  2) If it's configured as we need, the quit
#  3) If we need to change it, then read the whole memory page (256 byte) and save it to an array
#  4) Clear the page
#  5) Change the first bit in the first byte of saved memory page as we need
#  6) Write the page back to memory

proc setMemType {type} {
    global userflashbase 0xA0022000
    global fmareg 0x00
    global fmdreg 0x04
    global fmcreg 0x08
    global fcisreg 0x0c
    global fcicreg 0x14

    echo "Setting memory type..."
    clearFCIS

    ### Write addr to FMA (0x0) and set "READ_IFB" command
    ### Then wait for "Operation Done" in FCIS (status) register and then clear it

    mwh [expr $userflashbase + $fmareg] 0x0
    mww [expr $userflashbase + $fmcreg] 0xA4420040

    set waitTime 0
    while {[getFCIS] == 0} {
        set waitTime [expr $waitTime + 1]
        if {$waitTime > 10} {
            echo "Error while trying to map memory"
            return
        }
        echo "Waiting... $waitTime"
    }

    if {[getFCIS] == 0x3} {
        echo "Error while trying to map memory"
        return
    }
    clearFCIS

    # Read result to the variable "fmdData" and tell user whether MAIN or INFO block is mapped now

    set fmdData ""
    mem2array fmdData 8 0xA0022004 1

    if {[expr $fmdData(0) & 0x01] == 0} {
        if {[string equal $type true]} {
            echo "INFO block is already memory mapped."
            return
        } else {
            echo "INFO block is now mapped. Mapping MAIN block."
        }
    } else {
        if {[string equal $type false]} {
            echo "MAIN block is already memory mapped."
           return
        } else {
            echo "MAIN block is now mapped. Mapping INFO block."
        }
    }

    # If we are here, this means that we need to change the bit

    echo "Preserve memory page..."
    for {set i 0} {$i < 256} {incr $i} {
        set memPage($i) [readFlashWord $i]
    }

    if {[string equal $type true] } {
        set memPage(0) [expr $memPage(0) & 0xFE]
    } else {
        set memPage(0) [expr $memPage(0) | 0x01]
    }

    echo "Page saved. Clear page..."
    clearPage 0

    echo "Page cleared. Rewrite page..."
    for {set i 0} {$i < 256} {incr $i} {
        set memPage($i) [writeFlashWord $i $memPage($i)]
    }

    echo "Finished."
}