SECTIONS
{
	.text :
	{
		*(.text*)
		*(.rodata*)
	} > RAM

	__etext = .;

	.data : AT (__etext)
	{
		__data_start__ = .;
		*(.data*)
	} > RAM

	.bss :
	{
		. = ALIGN(4);
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end__ = .;
	} > RAM
}
