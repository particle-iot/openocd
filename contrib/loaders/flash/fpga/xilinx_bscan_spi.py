#!/usr/bin/python3
#
#  Copyright (C) 2015 Robert Jordens <jordens@gmail.com>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#

from migen.fhdl.std import *
from mibuild.generic_platform import *
from mibuild.xilinx import XilinxPlatform


"""
This migen script produces proxy bitstreams to allow programming SPI flashes
behind FPGAs. JTAG signalling is connected directly to SPI signalling. CS_N is
asserted when the JTAG IR contains the USER1 instruction and the state is
SHIFT-DR.

Xilinx bscan cells sample TDO on falling TCK and forward it.
MISO requires sampling on rising CLK and leads to one cycle of latency.

https://github.com/m-labs/migen
"""


class Spartan6(Module):
    def __init__(self, platform):
        self.clock_domains.cd_jtag = ClockDomain(reset_less=True)
        spi = platform.request("spiflash")
        shift = Signal()
        tdo = Signal()
        rst = Signal()
        self.comb += self.cd_jtag.clk.eq(spi.clk), spi.cs_n.eq(~shift | rst)
        self.sync.jtag += tdo.eq(spi.miso)
        self.specials += Instance("BSCAN_SPARTAN6", p_JTAG_CHAIN=1,
                                  o_TCK=spi.clk, o_SHIFT=shift, o_RESET=rst,
                                  o_TDI=spi.mosi, i_TDO=tdo)
        try:
            self.comb += platform.request("user_led", 0).eq(1)
            self.comb += platform.request("user_led", 1).eq(shift)
        except ConstraintError:
            pass


class Series7(Module):
    def __init__(self, platform):
        self.clock_domains.cd_jtag = ClockDomain(reset_less=True)
        spi = platform.request("spiflash")
        clk = Signal()
        shift = Signal()
        tdo = Signal()
        rst = Signal()
        self.comb += self.cd_jtag.clk.eq(clk), spi.cs_n.eq(~shift | rst)
        self.sync.jtag += tdo.eq(spi.miso)
        self.specials += Instance("BSCANE2", p_JTAG_CHAIN=1,
                                  o_SHIFT=shift, o_TCK=clk, o_RESET=rst,
                                  o_TDI=spi.mosi, i_TDO=tdo)
        self.specials += Instance("STARTUPE2", i_CLK=0, i_GSR=0, i_GTS=0,
                                  i_KEYCLEARB=0, i_PACK=1, i_USRCCLKO=clk,
                                  i_USRCCLKTS=0, i_USRDONEO=1, i_USRDONETS=1)
        try:
            self.comb += platform.request("user_led", 0).eq(1)
            self.comb += platform.request("user_led", 1).eq(shift)
        except ConstraintError:
            pass


class XilinxBscanSpi(XilinxPlatform):
    pinouts = {
        #                    cs_n, clk, mosi, miso, *pullups
        "xc6slx45-csg324-2": (["V3", "R15", "T13", "R13", "T14", "V14"],
                              "LVCMOS33", Spartan6),
        "xc6slx9-tqg144-2": (["P38", "P70", "P64", "P65", "P62", "P61"],
                             "LVCMOS33", Spartan6),
        "xc7k325t-ffg900-2": (["U19", None, "P24", "R25", "R20", "R21"],
                              "LVCMOS25", Series7),
    }

    def __init__(self, device, pins, std):
        cs_n, clk, mosi, miso = pins[:4]
        io = ["spiflash", 0,
              Subsignal("cs_n", Pins(cs_n)),
              Subsignal("mosi", Pins(mosi)),
              Subsignal("miso", Pins(miso), Misc("PULLUP")),
              IOStandard(std),
              ]
        if clk:
            io.append(Subsignal("clk", Pins(clk)))
        for i, p in enumerate(pins[4:]):
            io.append(Subsignal("pullup{}".format(i), Pins(p), Misc("PULLUP")))

        XilinxPlatform.__init__(self, device, [io])
        self.toolchain.bitgen_opt += " -g compress"

    @classmethod
    def make(cls, device):
        pins, std, Top = cls.pinouts[device]
        platform = cls(device, pins, std)
        top = Top(platform)
        name = "bscan_spi_{}".format(device)
        platform.build(top, build_name=name)


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description="build bscan_spi bitstreams "
                                "for openocd jtagspi flash driver")
    p.add_argument("device", nargs="*", default=list(XilinxBscanSpi.pinouts),
                   help="build for these devices (default: %(default)s)")
    args = p.parse_args()
    for device in args.device:
        XilinxBscanSpi.make(device)
