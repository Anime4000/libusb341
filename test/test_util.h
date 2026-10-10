/* libusb341 test helpers */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

static int t_failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		t_failures++; \
	} \
} while (0)

/* Same patterns as test/mock_ch341.c */
static inline uint8_t flash_pattern(uint32_t i)
{
	return (uint8_t)((i * 7u) ^ (i >> 8) ^ (i >> 16));
}

static inline uint8_t ee_pattern(int dev, uint32_t i)
{
	return (uint8_t)(0x5A ^ (i * 13u) ^ (uint32_t)dev);
}

/* stats: writes, reads, write_reads, ctrls, violations, spi bytes, i2c bytes, leftover packets */
static inline int mock_stats(unsigned long *s)
{
	typedef void (*fn_t)(unsigned long *);
	HMODULE m = GetModuleHandleA("libusb-1.0.dll");
	fn_t f = m ? (fn_t)(void *)GetProcAddress(m, "libusb341_mock_stats") : NULL;

	if (!f)
		return 0;
	f(s);
	return 1;
}

static inline void print_stats(void)
{
	unsigned long s[8];

	if (mock_stats(s))
		printf("  driver calls: %lu write, %lu read, %lu write/read, %lu control; "
			"%lu SPI bytes, %lu I2C bytes; violations %lu, leftover %lu\n",
			s[0], s[1], s[2], s[3], s[5], s[6], s[4], s[7]);
}

static inline int finish(const char *name)
{
	unsigned long s[8];

	if (mock_stats(s)) {
		print_stats();
		CHECK(s[4] == 0, "simulated chip saw %lu protocol violations", s[4]);
		CHECK(s[7] == 0, "%lu answer packets left unread in the chip", s[7]);
	}
	printf("%s: %s\n", name, t_failures ? "FAILED" : "OK");
	return t_failures ? 1 : 0;
}

static inline double now_ms(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#endif
