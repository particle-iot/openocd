# http://infocenter.arm.com/help/topic/com.arm.doc.ddi0464f/BABDIJAD.html
proc cache_a7_d_dump { } {
	# Set CSSELR to 0. This will switch CCSIDR to D-Cache mode
	arm mcr 15 2 0 0 0 0x0
	# Read CCSIDR to get I-Cache size
	set ccsidr [arm mrc 15 1 0 0 0]
	set numsets [expr ($ccsidr & 0x0fffe000)]
	set numsets [expr ($numsets >> 13)]

	set numways [expr ($ccsidr & 0x00001ff8)]
	set numways [expr ($numways >> 3) + 1]

	set numwords [expr ($ccsidr & 0x00000007)]
	set numwords [expr ( ($numwords + 2 ) << 2) ]

	echo [format "ways %i, lines %i, words in line: %i" $numways $numsets $numwords]
	# we read 2 words in one round.
	set numwordreads [expr ( $numwords >> 1) ]

	for { set way 0  } { $way < $numways } { incr $way } {

		# TODO: calculate index size
		for { set index 0  } { $index < $numsets } { incr $index } {

			set val [expr ($way << 30) | ($index << 6)]
			arm mcr 15 3 15 2 0 $val
			set tag0 [arm mrc 15 3 15 0 0]
			set tag1 [arm mrc 15 3 15 0 1]

			# valid cache?
			if { !($tag1) } {
				continue
			}

			set offset [expr ($tag1 & 0x1fffff)]
			set offset [expr ($offset << 11) + ($index << 6)]
			set step 4

			for { set word 0  } { $word < $numwordreads } { incr $word } {

				arm mcr 15 3 15 4 0 [expr $val | ($word << 3)]
				set dat0 [arm mrc 15 3 15 0 0]
				set dat1 [arm mrc 15 3 15 0 1]

				echo [format "wio: %i:%03i:%i offset: 0x%08x; data0: 0x%08x" \
					$way $index $word $offset $dat0]
				set offset [expr ($offset + $step)]

				echo [format "wio: %i:%03i:%i offset: 0x%08x; data1: 0x%08x" \
					$way $index $word $offset $dat1]
				set offset [expr ($offset + $step)]
			}
		}
	}
}

# http://infocenter.arm.com/help/topic/com.arm.doc.ddi0464f/BABDFFFC.html
proc cache_a7_i_dump { } {
	# Set CSSELR to 1. This will switch CCSIDR to I-Cache mode
	arm mcr 15 2 0 0 0 0x1
	# Read CCSIDR to get I-Cache size
	set ccsidr [arm mrc 15 1 0 0 0]
	set numsets [expr ($ccsidr & 0x0fffe000)]
	set numsets [expr ($numsets >> 13)]

	set numways [expr ($ccsidr & 0x00001ff8)]
	set numways [expr ($numways >> 3) + 1 ]

	set numwords [expr ($ccsidr & 0x00000007)]
	set numwords [expr ( ($numwords + 2 ) << 2) ]

	echo [format "ways %i, lines %i, words in line: %i" $numways $numsets $numwords]
	# we read 2 words in one round.
	set numwordreads [expr ( $numwords >> 1) ]

	for { set way 0  } { $way < $numways } { incr $way } {

		# TODO: calculate index size
		for { set index 0  } { $index < $numsets } { incr $index } {

			set val [expr ($way << 31) | ($index << 5)]
			arm mcr 15 3 15 2 1 $val
			set tag [arm mrc 15 3 15 0 0]
			set offset [expr ($tag & 0x0fffffff)]
			set offset [expr ($offset << 12) + ($index << 5)]

			# valid cache?
			if { !($tag & 0x40000000) } {
				continue
			}

			# ARM or Thumb?
			if { $tag & 0x20000000 } {
				set step 2
			} else {
				set step 4
			}

			for { set word 0  } { $word < $numwordreads } { incr $word } {

				arm mcr 15 3 15 4 1 [expr $val | ($word << 2)]
				set dat0 [arm mrc 15 3 15 0 0]
				set dat1 [arm mrc 15 3 15 0 1]

				echo [format "wio: %i:%03i:%i offset: 0x%08x; data0: 0x%08x" \
					$way $index $word $offset $dat0]
				set offset [expr ($offset + $step)]

				echo [format "wio: %i:%03i:%i offset: 0x%08x; data1: 0x%08x" \
					$way $index $word $offset $dat1]
				set offset [expr ($offset + $step)]
			}
		}
	}
}
