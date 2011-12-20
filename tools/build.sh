#!/bin/sh
INSTALL_DIR=/usr
if	! [ -f ./configure ] ; then
	echo	"configure missing, rebuilding"
	.	./bootstrap
else
	echo "Configure exists!"
fi

OCD_OPTIONS="--enable-maintainer-mode "
OCD_OPTIONS+="--enable-ft2232_libftdi "
OCD_OPTIONS+="--prefix=${INSTALL_DIR} "

echo	OCD_OPTIONS=\"${OCD_OPTIONS}\"
./configure  		${OCD_OPTIONS}

make

sudo make install

