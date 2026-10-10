/*
 * Drives IMSProg's unmodified CH341A SPI code (ch341a_spi.c: async bulk
 * transfers, 32 queued IN transfers) the way spi_nor_flash.c does.
 */
#include <string.h>
#include "ch341a_spi.h"
#include "test_util.h"

int mw_gpio_init(void) { return 0; }

static int cs_low(void) { return enable_pins(true); }
static int cs_high(void) { return enable_pins(false); }

static int cmd(const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	int ret = 0;
	cs_low();
	if (wn)
		ret |= ch341a_spi_send_command(wn, 0, w, NULL);
	if (rn)
		ret |= ch341a_spi_send_command(0, rn, NULL, r);
	cs_high();
	return ret;
}

static int flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t c[4] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
	return cmd(c, 4, buf, len);
}

static int wait_ready(void)
{
	uint8_t c = 0x05, sr = 0xff;
	int i;
	for (i = 0; i < 100; i++) {
		if (cmd(&c, 1, &sr, 1))
			return -1;
		if (!(sr & 1))
			return 0;
	}
	return -1;
}

static int verify(uint32_t addr, const uint8_t *buf, uint32_t len, const char *what)
{
	uint32_t i;
	for (i = 0; i < len; i++)
		if (buf[i] != flash_pattern(addr + i)) {
			printf("  %s: mismatch at 0x%06x: got %02x want %02x\n", what, addr + i, buf[i],
				flash_pattern(addr + i));
			return -1;
		}
	return 0;
}

int main(void)
{
	static uint8_t buf[1 << 20];
	uint8_t id[3] = { 0 }, c;
	const uint32_t odd[] = { 1, 2, 30, 31, 32, 61, 62, 63, 93, 1000, 3937, 3938, 4096, 65535 };
	unsigned i;
	double t0;
	int r;

	setvbuf(stdout, NULL, _IONBF, 0);
	r = ch341a_init(0, 1600);
	CHECK(r == 0, "ch341a_init returned %d", r);
	if (r)
		return finish("test_spi");

	c = 0x9F;
	r = cmd(&c, 1, id, 3);
	CHECK(r == 0 && id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18,
		"JEDEC id %02x %02x %02x (r=%d)", id[0], id[1], id[2], r);
	printf("  JEDEC ID %02X %02X %02X\n", id[0], id[1], id[2]);

	/* 1 MiB in 64 KiB sectors, as IMSProg reads */
	t0 = now_ms();
	for (i = 0; i < 16; i++) {
		r = flash_read(i * 0x10000, buf + i * 0x10000, 0x10000);
		CHECK(r == 0, "read sector %u: %d", i, r);
	}
	printf("  read 1 MiB in %.0f ms (simulated chip, no USB latency)\n", now_ms() - t0);
	CHECK(verify(0, buf, 1 << 20, "1 MiB read") == 0, "data mismatch");

	for (i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
		uint32_t a = 0x123457 + i * 0x1111;
		memset(buf, 0, odd[i]);
		r = flash_read(a, buf, odd[i]);
		CHECK(r == 0 && verify(a, buf, odd[i], "odd length") == 0, "read %u bytes at 0x%x", odd[i], a);
	}

	/* erase + program a page, read back */
	c = 0x06;
	cmd(&c, 1, NULL, 0);
	{
		uint8_t e[4] = { 0x20, 0x08, 0x00, 0x00 };
		cmd(e, 4, NULL, 0);
	}
	CHECK(wait_ready() == 0, "busy after erase");
	r = flash_read(0x080000, buf, 4096);
	for (i = 0; i < 4096 && buf[i] == 0xFF; i++)
		;
	CHECK(r == 0 && i == 4096, "sector not erased at +%u", i);
	{
		uint8_t pp[4 + 256];
		pp[0] = 0x02; pp[1] = 0x08; pp[2] = 0x01; pp[3] = 0x00;
		for (i = 0; i < 256; i++)
			pp[4 + i] = (uint8_t)(255 - i);
		c = 0x06;
		cmd(&c, 1, NULL, 0);
		r = cmd(pp, sizeof(pp), NULL, 0);
		CHECK(r == 0, "page program %d", r);
		CHECK(wait_ready() == 0, "busy after program");
		r = flash_read(0x080100, buf, 256);
		for (i = 0; i < 256 && buf[i] == (uint8_t)(255 - i); i++)
			;
		CHECK(r == 0 && i == 256, "programmed page mismatch at %u", i);
	}

	r = ch341a_spi_shutdown();
	CHECK(r == 0, "shutdown %d", r);
	return finish("test_spi");
}
