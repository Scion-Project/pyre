/*
 * Apple SEP: recognise a SEP ROM that can only idle.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/sep/rom-idle.h"

#define A64_NOP   (0xD503201FU)
#define A64_YIELD (0xD503203FU)
#define A64_WFE   (0xD503205FU)
#define A64_WFI   (0xD503207FU)

#define A64_B_MASK  (0xFC000000U)
#define A64_B_VALUE (0x14000000U)

static uint32_t rom_insn(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool apple_sep_rom_is_idle_loop(const void* rom, size_t size)
{
    const uint8_t* bytes = rom;
    size_t         n     = size / 4;
    size_t         pc    = 0;    // in instructions
    size_t         step;

    if (rom == NULL || n == 0) { return false; }

    // The path is deterministic, so after n + 1 steps without leaving the
    // allowed set it has revisited an instruction: it is a loop.
    for (step = 0; step <= n; ++step) {
        uint32_t insn = rom_insn(bytes + pc * 4);

        switch (insn) {
            case A64_NOP:
            case A64_YIELD:
            case A64_WFE:
            case A64_WFI: pc += 1; break;
            default:
                if ((insn & A64_B_MASK) == A64_B_VALUE) {
                    // imm26, sign-extended, in instructions.
                    int64_t off = (int64_t)((insn & 0x03FFFFFFU) ^ 0x02000000U) - 0x02000000;
                    if (off < 0 && (size_t)-off > pc) { return false; }
                    pc = (size_t)((int64_t)pc + off);
                    break;
                }
                return false;
        }
        if (pc >= n) { return false; }
    }
    return true;
}
