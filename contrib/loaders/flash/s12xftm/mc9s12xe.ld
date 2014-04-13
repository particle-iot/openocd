OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12",
	      "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(main)

/* Fixed definition of the available memory banks.
   See generic emulation script for a user defined configuration.  */

/* Setup the stack on the top of the data memory bank.  */
PROVIDE (_stack = 0x01100 + 0x6F00 - 1);
SECTIONS
{
  /* Concatenate .page0 sections.  Put them in the page0 memory bank
     unless we are creating a relocatable file.  */
  .page0 :
  {
    *(.page0)
  }  > RAM
  .text  :
  {
    *(.text)
    *(.text.*)
    . = ALIGN(2);
  }  > RAM
}
