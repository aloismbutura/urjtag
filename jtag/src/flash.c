/*
 * $Id$
 *
 * Copyright (C) 2002 ETC s.r.o.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by Marcel Telka <marcel@telka.sk>, 2002.
 *
 * Documentation:
 * [1] Advanced Micro Devices, "Common Flash Memory Interface Specification Release 2.0",
 *     December 1, 2001
 * [2] Intel Corporation, "Intel PXA250 and PXA210 Application Processors
 *     Developer's Manual", February 2002, Order Number: 278522-001
 * [3] Intel Corporation, "Common Flash Interface (CFI) and Command Sets
 *     Application Note 646", April 2000, Order Number: 292204-004
 * [4] Advanced Micro Devices, "Common Flash Memory Interface Publication 100 Vendor & Device
 *     ID Code Assignments", December 1, 2001, Volume Number: 96.1
 *
 */

#include "sysdep.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <flash/cfi.h>
#include <flash/intel.h>
#include <brux/cfi.h>

#include "bus.h"
#include "flash.h"
#include "jtag.h"

extern flash_driver_t amd_32_flash_driver;
extern flash_driver_t amd_8_flash_driver;
extern flash_driver_t intel_32_flash_driver;
extern flash_driver_t intel_16_flash_driver;
extern flash_driver_t intel_8_flash_driver;

flash_driver_t *flash_drivers[] = {
	&amd_32_flash_driver,
	&amd_8_flash_driver,
	&intel_32_flash_driver,
	&intel_16_flash_driver,
	&intel_8_flash_driver,
	NULL
};

flash_driver_t *flash_driver = NULL;

static void
set_flash_driver( cfi_array_t *cfi_array )
{
	int i;
	cfi_query_structure_t *cfi = &cfi_array->cfi_chips[0]->cfi;

	flash_driver = NULL;

	for (i = 0; flash_drivers[i] != NULL; i++)
		if (flash_drivers[i]->autodetect( cfi_array )) {
			flash_driver = flash_drivers[i];
			return;
		}

	printf( _("unknown flash - vendor id: %d (0x%04x)\n"),
		cfi->identification_string.pri_id_code,
		cfi->identification_string.pri_id_code );
}

/* check for flashmem - set driver */
static void
flashcheck( bus_t *bus, cfi_array_t **cfi_array )
{
	flash_driver = NULL;

	bus_prepare( bus );

	printf( _("Note: Supported configuration is 2 x 16 bit or 1 x 16 bit only\n") );

	*cfi_array = NULL;
	if (cfi_detect( bus, 0, cfi_array )) {
		cfi_array_free( *cfi_array );
		printf( _("Flash not found!\n") );
		return;
	}

	set_flash_driver( *cfi_array );
	if (!flash_driver) {
		printf( _("Flash not supported!\n") );
		return;
	}
	flash_driver->print_info( *cfi_array );
}

void
flashmsbin( bus_t *bus, FILE *f )
{
	uint32_t adr;
	cfi_query_structure_t *cfi;
	cfi_array_t *cfi_array;

	flashcheck( bus, &cfi_array );
	if (!cfi_array || !flash_driver) {
		printf( _("no flash driver found\n") );
		return;
	}
	cfi = &cfi_array->cfi_chips[0]->cfi;

	/* test sync bytes */
	{
		char sync[8];
		fread( &sync, sizeof (char), 7, f );
		sync[7] = '\0';
		if (strcmp( "B000FF\n", sync ) != 0) {
			printf( _("Invalid sync sequence!\n") );
			return;
		}
	}

	/* erase memory blocks */
	{
		uint32_t start;
		uint32_t len;
		int first, last;

		fread( &start, sizeof start, 1, f );
		fread( &len, sizeof len, 1, f );
		first = start / (cfi->device_geometry.erase_block_regions[0].erase_block_size * 2);
		last = (start + len - 1) / (cfi->device_geometry.erase_block_regions[0].erase_block_size * 2);
		for (; first <= last; first++) {
			adr = first * cfi->device_geometry.erase_block_regions[0].erase_block_size * 2;
			flash_driver->unlock_block( cfi_array, adr );
			printf( _("block %d unlocked\n"), first );
			printf( _("erasing block %d: %d\n"), first, flash_driver->erase_block( cfi_array, adr ) );
		}
	}

	printf( _("program:\n") );
	for (;;) {
		uint32_t a, l, c;

		fread( &a, sizeof a, 1, f );
		fread( &l, sizeof l, 1, f );
		fread( &c, sizeof c, 1, f );
		if (feof( f )) {
			printf( _("Error: premature end of file\n") );
			return;
		}
		printf( _("record: start = 0x%08X, len = 0x%08X, checksum = 0x%08X\n"), a, l, c );
		if ((a == 0) && (c == 0))
			break;
		if (l & 3) {
			printf( _("Error: Invalid record length!\n") );
			return;
		}

		while (l) {
			uint32_t data;

			printf( _("addr: 0x%08X"), a );
			printf( "\r" );
			fflush(stdout);
			fread( &data, sizeof data, 1, f );
			if (flash_driver->program( cfi_array, a, data )) {
				printf( _("\nflash error\n") );
				return;
			}
			a += 4;
			l -= 4;
		}
	}
	printf( "\n" );

	flash_driver->readarray( cfi_array );

	fseek( f, 15, SEEK_SET );
	printf( _("verify:\n") );

	for (;;) {
		uint32_t a, l, c;

		fread( &a, sizeof a, 1, f );
		fread( &l, sizeof l, 1, f );
		fread( &c, sizeof c, 1, f );
		if (feof( f )) {
			printf( _("Error: premature end of file\n") );
			return;
		}
		printf( _("record: start = 0x%08X, len = 0x%08X, checksum = 0x%08X\n"), a, l, c );
		if ((a == 0) && (c == 0))
			break;
		if (l & 3) {
			printf( _("Error: Invalid record length!\n") );
			return;
		}

		while (l) {
			uint32_t data, readed;

			printf( _("addr: 0x%08X"), a );
			printf( "\r" );
			fflush( stdout );
			fread( &data, sizeof data, 1, f );
			readed = bus_read( bus, a );
			if (data != readed) {
				printf( _("\nverify error: 0x%08X vs. 0x%08X at addr %08X\n"),
					readed, data, a );
				return;
			}
			a += 4;
			l -= 4;
		}
	}

	printf( _("\nDone.\n") );

	cfi_array_free( cfi_array );
}

void
flashmem( bus_t *bus, FILE *f, uint32_t addr )
{
	uint32_t adr;
	cfi_query_structure_t *cfi;
	cfi_array_t *cfi_array;
	int *erased;
	int i;

	flashcheck( bus, &cfi_array );
	if (!cfi_array || !flash_driver) {
		printf( _("no flash driver found\n") );
		return;
	}
	cfi = &cfi_array->cfi_chips[0]->cfi;

	erased = malloc( cfi->device_geometry.erase_block_regions[0].number_of_erase_blocks * sizeof *erased );
	if (!erased) {
		printf( _("Out of memory!\n") );
		return;
	}
	for (i = 0; i < cfi->device_geometry.erase_block_regions[0].number_of_erase_blocks; i++)
		erased[i] = 0;

	printf( _("program:\n") );
	adr = addr;
	while (!feof( f )) {
		uint32_t data;
#define BSIZE 4096
		uint8_t b[BSIZE];
		int bc = 0, bn = 0;
		int block_no = adr / (cfi->device_geometry.erase_block_regions[0].erase_block_size * flash_driver->bus_width / 2);

		if (!erased[block_no]) {
			flash_driver->unlock_block( cfi_array, adr );
			printf( _("\nblock %d unlocked\n"), block_no );
			printf( _("erasing block %d: %d\n"), block_no, flash_driver->erase_block( cfi_array, adr ) );
			erased[block_no] = 1;
		}

		bn = fread( b, 1, BSIZE, f );
		for (bc = 0; bc < bn; bc += flash_driver->bus_width) {
			int j;
			printf( _("addr: 0x%08X"), adr );
			printf( "\r" );
			fflush( stdout );

			data = 0;
			for (j = 0; j < flash_driver->bus_width; j++)
				if (big_endian)
					data = (data << 8) | b[bc + j];
				else
					data |= b[bc + j] << (j * 8);

			if (flash_driver->program( cfi_array, adr, data )) {
				printf( _("\nflash error\n") );
				return;
			}
			adr += flash_driver->bus_width;
		}
	}
	printf( "\n" );

	flash_driver->readarray( cfi_array );

	fseek( f, 0, SEEK_SET );
	printf( _("verify:\n") );
	fflush( stdout );
	adr = addr;
	while (!feof( f )) {
		uint8_t buf[16];
		uint32_t data;
		uint32_t readed;
		int j;

		if (fread( buf, flash_driver->bus_width, 1, f ) != 1) {
			if (feof(f))
				break;
			printf( _("Error during file read.\n") );
			return;
		}

		data = 0;
		for (j = 0; j < flash_driver->bus_width; j++)
			if (big_endian)
				data = (data << 8) | buf[j];
			else
				data |= buf[j] << (j * 8);

		printf( _("addr: 0x%08X"), adr );
		printf( "\r" );
		fflush( stdout );
		readed = bus_read( bus, adr );
		if (data != readed) {
			printf( _("\nverify error:\nreaded: 0x%08X\nexpected: 0x%08X\n"), readed, data );
			return;
		}
		adr += flash_driver->bus_width;
	}
	printf( _("\nDone.\n") );

	free( erased );

	cfi_array_free( cfi_array );
}

void
flasherase( bus_t *bus, uint32_t addr, int number )
{
	cfi_query_structure_t *cfi;
	cfi_array_t *cfi_array;
	int i;

	printf( _("addr: 0x%08X\n"), addr);

	flashcheck( bus, &cfi_array );
	if (!cfi_array || !flash_driver) {
		printf( _("no flash driver found\n") );
		return;
	}
	cfi = &cfi_array->cfi_chips[0]->cfi;

	printf( _("program:\n") );
	for (i = 1; i <= number; i++) {
		int addr_block = (cfi->device_geometry.erase_block_regions[0].erase_block_size * flash_driver->bus_width / 2);
		int block_no = addr / addr_block;
		printf( _("addr: 0x%08X\n"), addr);
		fflush(stdout);
		flash_driver->unlock_block( cfi_array, addr );
		printf( _("block %d unlocked\n"), block_no );
		printf( _("erasing block %d: %d\n"), block_no, flash_driver->erase_block( cfi_array, addr ) );

		addr |= (addr_block - 1);
		addr += 1;
	}
	printf( _("\nDone.\n") );

	cfi_array_free( cfi_array );
	/* BYPASS */
	//       parts_set_instruction( ps, "BYPASS" );
	//       chain_shift_instructions( chain );
}
