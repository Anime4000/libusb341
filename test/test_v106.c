/*
 * Drives the CH341 code compiled into the shipped SFP-Master.exe (git tag
 * v1.0.6-win64, ch341funcs.c, unmodified): asynchronous EEPROM reads with the
 * IN transfer submitted before the OUT transfer, 300 ms timeouts.
 */
#include <string.h>
#include "test_util.h"

FILE *debugout, *verbout;

int32_t ch341aConnect(void);
int32_t ch341aShutdown(void);
int32_t ch341readEEPROM_param(uint8_t *buffer, uint32_t offset, uint32_t bytestoread,
	uint32_t ic_size, uint32_t block_size, uint8_t algorithm);
int32_t ch341writeEEPROM_param(uint8_t *buffer, uint32_t offset, uint32_t bytesum,
	uint32_t block_size, uint8_t algorithm);

int main(void)
{
	uint8_t buf[512], w[16];
	int r, i;

	setvbuf(stdout, NULL, _IONBF, 0);
	debugout = verbout = stdout;
	r = ch341aConnect();
	CHECK(r == 0, "ch341aConnect returned %d", r);
	if (r)
		return finish("test_v106");

	memset(buf, 0, sizeof(buf));
	r = ch341readEEPROM_param(buf, 0, 256, 256, 8, 0x01);
	CHECK(r == 0, "async read: %d", r);
	for (i = 0; i < 256 && buf[i] == ee_pattern(0, (uint32_t)i); i++)
		;
	CHECK(i == 256, "mismatch at %d: %02x want %02x", i, buf[i], ee_pattern(0, (uint32_t)i));

	for (i = 0; i < 16; i++)
		w[i] = (uint8_t)(0xA0 + i);
	r = ch341writeEEPROM_param(w, 0x20, 16, 8, 0x01);
	CHECK(r == 0, "write: %d", r);
	memset(buf, 0, sizeof(buf));
	r = ch341readEEPROM_param(buf, 0, 256, 256, 8, 0x01);
	CHECK(r == 0, "read back: %d", r);
	CHECK(memcmp(buf + 0x20, w, 16) == 0, "written bytes not read back");

	r = ch341aShutdown();
	CHECK(r == 0, "shutdown %d", r);
	return finish("test_v106");
}
