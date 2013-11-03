#ifndef _FIRMWARE_H
#define _FIRMWARE_H

struct ctrl_payload {
	unsigned short address;
	int len;
	char data[50];
};

#endif
