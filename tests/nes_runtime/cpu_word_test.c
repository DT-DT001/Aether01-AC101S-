#include <assert.h>
#include <stdio.h>
#include "../../components/aether_nes/vendor/nofrendo/cpu/nes6502.c"

int main(void)
{
    uint8 *pages[NES6502_NUMBANKS];
    for (unsigned i = 0; i < NES6502_NUMBANKS; ++i) {
        pages[i] = calloc(1, NES6502_BANKSIZE);
        assert(pages[i]);
        cpu.mem_page[i] = pages[i];
    }
    ram = pages[0];
    ram[1] = 0x34; ram[2] = 0x12;
    assert(zp_readword(1) == 0x1234); /* Odd address has no alignment contract. */
    ram[255] = 0x78; ram[0] = 0x56;
    assert(zp_readword(255) == 0x5678);
    pages[8][NES6502_BANKSIZE - 1] = 0xcd;
    pages[9][0] = 0xab;
    assert(bank_readword(0x8fff) == 0xabcd); /* Separate backing allocations. */
    pages[15][NES6502_BANKSIZE - 1] = 0x10;
    pages[0][0] = 0x32;
    assert(bank_readword(0xffff) == 0x3210);
    for (unsigned i = 0; i < NES6502_NUMBANKS; ++i) free(pages[i]);
    puts("NES production CPU word reads: alignment/zero-page/bank/16-bit wrap passed");
    return 0;
}
