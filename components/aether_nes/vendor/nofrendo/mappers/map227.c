/* Aether01, 2026-11-14: Removed unused snapshot callback entries. */
/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** map227.c
**
** iNES mapper 227: 1200-in-1 / 600-in-1 style multicarts
**
** The register is the CPU ADDRESS of any write to $8000-$FFFF
** (the written value is ignored):
**
**   A0      (S) : with O=1, 0 = NROM-128 (16KB mirrored), 1 = NROM-256 (32KB)
**   A1      (M) : nametable mirroring, 0 = vertical, 1 = horizontal
**   A2-A6       : inner PRG bank p (16KB units), A8 extends to p bit 5
**   A7      (O) : 1 = NROM mode (game selected), 0 = UNROM-like menu mode
**   A9      (L) : with O=0 selects the fixed bank at $C000:
**                 0 = bank 0 of the current 128KB block, 1 = bank 7 (UNROM)
**
** Power-on latch is 0 -> 16KB bank 0 at both $8000 and $C000, which is
** where the multicart menu lives, so the menu boots automatically.
** Banking formulas follow the NESdev wiki / FCEUX implementation.
*/

#include <noftypes.h>
#include <nes_mmc.h>
#include <nes_ppu.h>

static uint32 map227_latch = 0;

static void map227_sync(void)
{
   int p = (int) (((map227_latch >> 2) & 0x1F) | ((map227_latch & 0x100) >> 3));

   if (map227_latch & 0x80)         /* O=1: game mode (NROM) */
   {
      if (map227_latch & 0x01)      /* S=1: 32KB NROM-256 */
      {
         mmc_bankrom(32, 0x8000, p >> 1);
      }
      else                          /* S=0: 16KB NROM-128, mirrored */
      {
         mmc_bankrom(16, 0x8000, p);
         mmc_bankrom(16, 0xC000, p);
      }
   }
   else                             /* O=0: menu / UNROM-like mode */
   {
      mmc_bankrom(16, 0x8000, (map227_latch & 0x01) ? (p & 0x3E) : p);
      mmc_bankrom(16, 0xC000, (map227_latch & 0x200) ? (p | 0x07) : (p & 0x38));
   }

   if (map227_latch & 0x02)
      ppu_mirror(0, 0, 1, 1);       /* horizontal */
   else
      ppu_mirror(0, 1, 0, 1);       /* vertical */
}

static void map227_init(void)
{
   map227_latch = 0;
   map227_sync();
}

static void map227_write(uint32 address, uint8 value)
{
   UNUSED(value);

   map227_latch = address;
   map227_sync();
}

static map_memwrite map227_memwrite[] =
{
   { 0x8000, 0xFFFF, map227_write },
   {     -1,     -1, NULL }
};

mapintf_t map227_intf =
{
   227, /* mapper number */
   "1200-in-1", /* mapper name */
   map227_init, /* init routine */
   NULL, /* vblank callback */
   NULL, /* hblank callback */
   NULL, /* memory read structure */
   map227_memwrite, /* memory write structure */
   NULL /* external sound device */
};
