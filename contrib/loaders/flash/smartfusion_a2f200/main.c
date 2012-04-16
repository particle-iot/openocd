#include <mss_nvm.h>

register uint32_t start_r asm ("r0");
register uint8_t * p_data_r asm ("r1");
register size_t n_bytes_r asm ("r2");

int main() {
	uint32_t start = start_r;
	uint8_t * p_data = p_data_r;
	size_t n_bytes = n_bytes_r;
	NVM_init(NVM_A2F200_DEVICE);
	return NVM_write (start, p_data, n_bytes);
}
