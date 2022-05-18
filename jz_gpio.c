/*
    This file is part of jz-diag-tools.
    Copyright (C) 2022 Reimu NotMoe <reimu@sudomaker.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ctype.h>

static void show_help() {
	puts(
		"Usage: jz_gpio <show|[GPIO_DEF [COMMAND VALUE]]>\n"
		"GPIO diagnostic tool for Ingenic SoCs.\n"
		"\n"
		"Commands:\n"
		"  inl                        Read input level\n"
		"  int                        Set interrupt\n"
		"  msk                        Set mask\n"
		"  pat0                       Set pattern 0 (data)\n"
		"  pat1                       Set pattern 1 (direction)\n"
		"  gpio_input                 Shortcut of `int 0', `msk 1', `pat1 1'\n"
		"  gpio_output                Shortcut of `int 0', `msk 1', `pat1 0'\n"
		"  read                       Shortcut of `inl'\n"
		"  write                      Shortcut of `pat0'\n"
		"  func                       Shortcut of `int 0', `msk 0', `pat1 <1>', `pat0 <0>'\n"
		"\n"
		"Examples:\n"
		"  jz_gpio show\n"
		"  jz_gpio pc23 input\n"
		"  jz_gpio pc23 read\n"
		"  jz_gpio pa00 output\n"
		"  jz_gpio pa00 write 1\n"
		"  jz_gpio pd00 func 0  # Set PD00 as ssi0_clk on X1000\n"
		"  jz_gpio pd00 gpio    # Revert to GPIO mode\n"
	);
}

#define GPIO_BASE		0x10010000
#define GPIO_PORT_WIDTH		0x100

#define BIT_GET(x, n)		(((x) >> (n)) & 1)
#define BIT_SET(x, n)		((x) |= (1 << (n)))
#define BIT_CLR(x, n)		((x) &= ~(1 << (n)))

typedef struct {
	volatile const uint32_t INL;
	volatile const uint32_t _rsvd0[3];
	volatile uint32_t INT;
	volatile uint32_t INTS;
	volatile uint32_t INTC;
	volatile const uint32_t _rsvd1[1];
	volatile uint32_t MSK;
	volatile uint32_t MSKS;
	volatile uint32_t MSKC;
	volatile const uint32_t _rsvd2[1];
	volatile uint32_t PAT1;
	volatile uint32_t PAT1S;
	volatile uint32_t PAT1C;
	volatile const uint32_t _rsvd3[1];
	volatile uint32_t PAT0;
	volatile uint32_t PAT0S;
	volatile uint32_t PAT0C;
	volatile const uint32_t _rsvd4[1];
	volatile const uint32_t FLG;
	volatile const uint32_t _rsvd5[1];
	volatile const uint32_t FLGC;
	volatile const uint32_t _rsvd6[5];
	volatile uint32_t PEN;
	volatile uint32_t PENS;
	volatile uint32_t PENC;
	volatile const uint32_t _rsvd7[29];
	volatile uint32_t GID2LD;
} XHAL_GPIO_HandleTypeDef;

static void *phys_mem = NULL;

static void show_gpios() {
	for (int i=0; i<7; i++) {
		volatile XHAL_GPIO_HandleTypeDef *port = phys_mem + i * GPIO_PORT_WIDTH;

		printf("Port %c\n", 'A' + i);
		printf("================\n");

		for (int j=0; j<32; j++) {
			printf("P%c%02u: ", 'A' + i, j);

			bool b_int = BIT_GET(port->INT, j);
			bool b_msk = BIT_GET(port->MSK, j);
			bool b_pat1 = BIT_GET(port->PAT1, j);
			bool b_pat0 = BIT_GET(port->PAT0, j);

			if (b_int) {
				printf("INTERRUPT ");

				if (b_pat1) {
					if (b_pat0) {
						printf("RISING");
					} else {
						printf("FALLING");
					}
					printf("_EDGE ");
				} else {
					if (b_pat0) {
						printf("HIGH");
					} else {
						printf("LOW");
					}
					printf("_LEVEL ");
				}

				if (b_msk) {
					printf("DISABLED\n");
				} else {
					printf("ENABLED\n");
				}
			} else {
				if (b_msk) {
					printf("GPIO ");

					if (b_pat1) {
						bool b_inl = BIT_GET(port->INL, j);
						printf("INPUT %u\n", b_inl);

					} else {
						printf("OUTPUT %u\n", b_pat0);
					}

				} else {
					printf("FUNCTION %d\n", b_pat1 << 1 | b_pat0);
				}
			}
		}

		printf("\n");
	}
}

static bool str2portoff(const char *str, void **port, uint8_t *offset) {
	if (strlen(str) != 4) {
		return false;
	}

	uint8_t portchar = toupper(str[1]);
	if (portchar < 'A' || portchar > 'G') {
		return false;
	}

	uint8_t off = strtol(str+2, NULL, 10);
	if (off > 31) {
		return false;
	}

	*port = phys_mem + (portchar - 'A') * GPIO_PORT_WIDTH;
	*offset = off;

	return true;
}

static void gpio_read_inl(void *port_addr, uint8_t offset) {
	volatile XHAL_GPIO_HandleTypeDef *port = port_addr;

	printf("%u\n", BIT_GET(port->INL, offset));
}

static long check_val(const char *val) {
	if (!val) {
		printf("error: value not specified");
		exit(2);
	}

	return strtol(val, NULL, 10);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		show_help();
		return 1;
	}

	int fd = open("/dev/mem", O_RDWR|O_SYNC);

	if (fd < 0) {
		perror("error: failed to open /dev/mem");
		return 2;
	}

	phys_mem = mmap(NULL, 0x10000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);

	if (phys_mem == MAP_FAILED) {
		perror("error: mmap failed");
		return 2;
	}

	if (0 == strcmp(argv[1], "show")) {
		show_gpios();
	} else {
		volatile XHAL_GPIO_HandleTypeDef *port;
		uint8_t offset;
		const char *val = argv[3];

		if (str2portoff(argv[1], (void **) &port, &offset)) {
			if (!argv[2]) {
				printf("error: no command specified\n");
				return 2;
			}

			if (0 == strcmp(argv[2], "inl") || 0 == strcmp(argv[2], "read")) {
				printf("%u\n", BIT_GET(port->INL, offset));
			} else if (0 == strcmp(argv[2], "int")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->INTS, offset);
				} else {
					BIT_SET(port->INTC, offset);
				}
			} else if (0 == strcmp(argv[2], "pat0") || 0 == strcmp(argv[2], "write")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->PAT0S, offset);
				} else {
					BIT_SET(port->PAT0C, offset);
				}
			} else if (0 == strcmp(argv[2], "pat1")) {
				uint8_t v = check_val(val);
				if (v) {
					BIT_SET(port->PAT1S, offset);
				} else {
					BIT_SET(port->PAT1C, offset);
				}
			} else if (0 == strcmp(argv[2], "gpio_input")) {
				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKS, offset);
				BIT_SET(port->PAT1S, offset);
			} else if (0 == strcmp(argv[2], "gpio_output")) {
				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKS, offset);
				BIT_SET(port->PAT1C, offset);
			} else if (0 == strcmp(argv[2], "func")) {
				uint8_t v = check_val(val);

				BIT_SET(port->INTC, offset);
				BIT_SET(port->MSKC, offset);

				BIT_GET(v, 1) ? BIT_SET(port->PAT1S, offset) : BIT_SET(port->PAT1C, offset);
				BIT_GET(v, 0) ? BIT_SET(port->PAT0S, offset) : BIT_SET(port->PAT0C, offset);
			} else {
				printf("error: Bad command `%s'\n", argv[2]);
			}
		} else {
			printf("error: Bad pin specification `%s'\n", argv[1]);
			return 2;
		}
	}

	return 0;
}
