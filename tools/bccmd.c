/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2005  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 *  CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
 *  COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
 *  SOFTWARE IS DISCLAIMED.
 *
 *
 *  $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "csr.h"

static int cmd_keylen(int dd, int argc, char *argv[])
{
	uint8_t buf[8];
	uint16_t handle, keylen;
	int err;

	if (argc < 1) {
		errno = EINVAL;
		return -1;
	}

	if (argc > 1) {
		errno = E2BIG;
		return -1;
	}

	handle = atoi(argv[0]);

	memset(buf, 0, sizeof(buf));
	buf[0] = handle & 0xff;
	buf[1] = handle >> 8;

	err = csr_read_varid_complex(dd, 0x4711,
				CSR_VARID_CRYPT_KEY_LENGTH, buf, sizeof(buf));
	if (err < 0) {
		errno = -err;
		return -1;
	}

	handle = buf[0] | (buf[1] << 8);
	keylen = buf[2] | (buf[3] << 8);

	printf("Crypt key length: %d bit\n", keylen * 8);

	return 0;
}

static int cmd_clock(int dd, int argc, char *argv[])
{
	uint32_t clock = 0;
	int err;

	err = csr_read_varid_uint32(dd, 0x4711, CSR_VARID_BT_CLOCK, &clock);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	printf("Bluetooth clock: 0x%04x (%d)\n", clock, clock);

	return 0;
}

static int cmd_panicarg(int dd, int argc, char *argv[])
{
	uint16_t error = 0;
	int err;

	err = csr_read_varid_uint16(dd, 5, CSR_VARID_PANIC_ARG, &error);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	printf("Panic code: 0x%02x (%s)\n", error,
					error < 0x100 ? "valid" : "invalid");

	return 0;
}

static int cmd_faultarg(int dd, int argc, char *argv[])
{
	uint16_t error = 0;
	int err;

	err = csr_read_varid_uint16(dd, 5, CSR_VARID_FAULT_ARG, &error);
	if (err < 0) {
		errno = -err;
		return -1;
	}

	printf("Fault code: 0x%02x (%s)\n", error,
					error < 0x100 ? "valid" : "invalid");

	return 0;
}

static struct {
	char *str;
	int (*func)(int dd, int argc, char **argv);
	char *arg;
	char *doc;
} commands[] = {
	{ "keylen",   cmd_keylen,   "<handle>", "Get current crypt key length" },
	{ "clock",    cmd_clock,    "",         "Get local Bluetooth clock"    },
	{ "panicarg", cmd_panicarg, "",         "Get panic code argument"      },
	{ "faultarg", cmd_faultarg, "",         "Get fault code argument"      },
	{ NULL },
};

static void usage(void)
{
	int i;

	printf("bccmd - Utility for the CSR BCCMD interface\n\n");
	printf("Usage:\n"
		"\tbccmd [-i <dev>] <command>\n\n");

	printf("Commands:\n");
		for (i = 0; commands[i].str; i++)
			printf("\t%-10s%-8s\t%s\n", commands[i].str,
				commands[i].arg, commands[i].doc);
}

static struct option main_options[] = {
	{ "help",	0, 0, 'h' },
	{ "device",	1, 0, 'i' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	struct hci_dev_info di;
	struct hci_version ver;
	int i, err, dd, opt, dev = 0;

	while ((opt=getopt_long(argc, argv, "+i:h", main_options, NULL)) != -1) {
		switch (opt) {
		case 'i':
			dev = hci_devid(optarg);
			if (dev < 0) {
				perror("Invalid device");
				exit(1);
			}
			break;

		case 'h':
		default:
			usage();
			exit(0);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		usage();
		exit(1);
	}

	dd = hci_open_dev(dev);
	if (dd < 0) {
		fprintf(stderr, "Can't open device hci%d: %s (%d)\n",
						dev, strerror(errno), errno);
		exit(1);
	}

	if (hci_devinfo(dev, &di) < 0) {
		fprintf(stderr, "Can't get device info for hci%d: %s (%d)\n",
						dev, strerror(errno), errno);
		hci_close_dev(dd);
		exit(1);
	}

	if (hci_read_local_version(dd, &ver, 1000) < 0) {
		fprintf(stderr, "Can't read version info for hci%d: %s (%d)\n",
						dev, strerror(errno), errno);
		hci_close_dev(dd);
		exit(1);
	}

	if (ver.manufacturer != 10) {
		fprintf(stderr, "Unsupported manufacturer\n");
		hci_close_dev(dd);
		exit(1);
	}

	for (i = 0; commands[i].str; i++) {
		if (strcasecmp(commands[i].str, argv[0]))
			continue;

		err = commands[i].func(dd, argc - 1, argv + 1);

		hci_close_dev(dd);

		if (err < 0) {
			fprintf(stderr, "Can't execute command: %s (%d)\n",
							strerror(errno), errno);
			exit(1);
		}

		exit(0);
	}

	fprintf(stderr, "Unsupported command\n");

	hci_close_dev(dd);

	exit(1);
}
