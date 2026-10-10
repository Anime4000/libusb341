/*
 * Drives SFP-Master's unmodified CH341 I2C code (ch34x_i2c.c: synchronous
 * bulk transfers) against the simulated SFP EEPROMs at A0h and A2h.
 */
#include <string.h>
#include "ch34x_i2c.h"
#include "test_util.h"

int main(void)
{
	uint8_t buf[512], w[32];
	int r, i;

	setvbuf(stdout, NULL, _IONBF, 0);
	r = ch341aConnect();
	CHECK(r == 0, "ch341aConnect returned %d", r);
	if (r)
		return finish("test_i2c");

	/* A0h page (24C02-style, 1-byte address) */
	memset(buf, 0, sizeof(buf));
	r = ch34xi2cBlockRead(buf, 0x000, 256, 0x01);
	CHECK(r == 0, "read A0h: %d", r);
	for (i = 0; i < 256 && buf[i] == ee_pattern(0, (uint32_t)i); i++)
		;
	CHECK(i == 256, "A0h mismatch at %d: %02x", i, buf[i]);

	/* A2h page: address bit 8 selects the second device */
	memset(buf, 0, sizeof(buf));
	r = ch34xi2cBlockRead(buf, 0x100, 256, 0x11);
	CHECK(r == 0, "read A2h: %d", r);
	for (i = 0; i < 256 && buf[i] == ee_pattern(1, (uint32_t)i); i++)
		;
	CHECK(i == 256, "A2h mismatch at %d: %02x", i, buf[i]);

	/* write 32 bytes in 8-byte pages, read back */
	for (i = 0; i < 32; i++)
		w[i] = (uint8_t)(0xC0 + i);
	r = ch34xi2cBlockWrite(w, 0x40, 32, 8, 0x01);
	CHECK(r == 0, "write: %d", r);
	memset(buf, 0, sizeof(buf));
	r = ch34xi2cBlockRead(buf, 0x000, 256, 0x01);
	CHECK(r == 0, "read back: %d", r);
	CHECK(memcmp(buf + 0x40, w, 32) == 0, "written bytes not read back");
	CHECK(buf[0x3F] == ee_pattern(0, 0x3F) && buf[0x60] == ee_pattern(0, 0x60), "neighbours changed");

	r = ch341aShutdown();
	CHECK(r == 0, "shutdown %d", r);
	return finish("test_i2c");
}
