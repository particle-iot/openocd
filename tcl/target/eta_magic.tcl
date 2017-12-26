
proc setmagic  { } {
    mww 0x1001FFF0 0xc001c0de
    mww 0x1001FFF4 0xc001c0de
    mww 0x1001FFF8 0xdeadbeef
    mww 0x1001FFFC 0xc369a517
    echo "Magic Bits Set"
}

proc clearmagic { } {
    mww 0x1001FFF0 0 4
    echo "Magic Bits Cleared"
}
