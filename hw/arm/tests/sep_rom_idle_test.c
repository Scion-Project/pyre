/*
 * Host-side test for apple_sep_rom_is_idle_loop() (hw/arm/sep/rom-idle.c).
 * Built and run by hw/arm/tests/run-host-tests.sh; no guest firmware needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/sep/rom-idle.h"

#define NOP   0xD503201FU
#define WFI   0xD503207FU
#define WFE   0xD503205FU
#define YIELD 0xD503203FU
#define SEV   0xD503209FU
#define MOVZ0 0xD2800000U /* mov x0, #0 */
#define MSR_DAIFCLR 0xD50342FFU /* msr daifclr, #2 */
#define BCOND 0x54000000U /* b.eq . */
#define BL0   0x94000000U /* bl . */

/* B with a displacement in instructions. */
static uint32_t b(int32_t insns) { return 0x14000000U | ((uint32_t)insns & 0x03FFFFFFU); }

static int failures;

static void put(uint8_t* rom, size_t idx, uint32_t insn)
{
    rom[idx * 4 + 0] = insn & 0xff;
    rom[idx * 4 + 1] = (insn >> 8) & 0xff;
    rom[idx * 4 + 2] = (insn >> 16) & 0xff;
    rom[idx * 4 + 3] = insn >> 24;
}

static void check(const char* name, const uint8_t* rom, size_t size, bool want)
{
    bool got = apple_sep_rom_is_idle_loop(rom, size);
    printf("%-4s %s (%s, want %s)\n", got == want ? "ok" : "FAIL", name, got ? "idle" : "not idle",
           want ? "idle" : "not idle");
    if (got != want) { failures++; }
}

/* Fill @n instructions by repeating @pat (@plen instructions). */
static uint8_t* rom_of(size_t n, const uint32_t* pat, size_t plen)
{
    uint8_t* rom = calloc(n, 4);
    for (size_t i = 0; i < n; i++) { put(rom, i, pat[i % plen]); }
    return rom;
}

/* `sep_rom_idle_test FILE...`: classify ROM images instead (no assertions). */
static int classify(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        FILE*    f = fopen(argv[i], "rb");
        uint8_t* buf;
        long     size;

        if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
            perror(argv[i]);
            return 2;
        }
        rewind(f);
        buf = malloc(size ? size : 1);
        if (fread(buf, 1, size, f) != (size_t)size) {
            perror(argv[i]);
            return 2;
        }
        fclose(f);
        printf("%s: %ld bytes, %s\n", argv[i], size,
               apple_sep_rom_is_idle_loop(buf, size) ? "idle loop (SEP core parked)" : "not an idle loop");
        free(buf);
    }
    return 0;
}

int main(int argc, char** argv)
{
    uint8_t* rom;

    if (argc > 1) { return classify(argc, argv); }

    /* The two placeholders the ios-on-pixel benches use (4 KiB each). */
    {
        uint32_t pat[] = { b(0) };
        rom = rom_of(1024, pat, 1);
        check("spin.seprom: `b .` x1024", rom, 4096, true);
        free(rom);
    }
    {
        uint32_t pat[] = { WFI, b(-1) };
        rom = rom_of(1024, pat, 2);
        check("wfi.seprom: `wfi; b .-4` x512", rom, 4096, true);
        free(rom);
    }
    {
        uint32_t pat[] = { NOP, YIELD, WFE, WFI, b(-4) };
        rom = rom_of(5, pat, 5);
        check("nop/yield/wfe/wfi loop", rom, 20, true);
        free(rom);
    }
    {
        uint32_t pat[] = { b(0) };
        rom = rom_of(1, pat, 1);
        check("single `b .`", rom, 4, true);
        free(rom);
        rom = calloc(1, 6);
        put(rom, 0, b(0));
        check("`b .` then 2 stray bytes", rom, 6, true);
        free(rom);
    }
    {
        /* Jump far ahead, idle there, come back: still only idles. */
        rom = rom_of(1024, (uint32_t[]) { MOVZ0 }, 1);
        put(rom, 0, b(512));
        put(rom, 512, WFI);
        put(rom, 513, b(-513));
        check("far branch into a WFI loop and back", rom, 4096, true);
        free(rom);
    }

    /* Not idle. */
    check("NULL", NULL, 4096, false);
    {
        uint32_t pat[] = { b(0) };
        rom = rom_of(1, pat, 1);
        check("empty", rom, 0, false);
        check("3 bytes", rom, 3, false);
        free(rom);
    }
    {
        uint32_t pat[] = { WFI };
        rom = rom_of(16, pat, 1);
        check("WFIs that run off the end", rom, 64, false);
        free(rom);
    }
    {
        uint32_t pat[] = { MOVZ0, b(0) };
        rom = rom_of(2, pat, 2);
        check("`mov x0, #0; b .`", rom, 8, false);
        free(rom);
    }
    {
        uint32_t pat[] = { MSR_DAIFCLR, WFI, b(-1) };
        rom = rom_of(3, pat, 3);
        check("unmasks IRQs before its WFI loop", rom, 12, false);
        free(rom);
    }
    {
        uint32_t pat[] = { SEV, b(-1) };
        rom = rom_of(2, pat, 2);
        check("SEV loop (signals other cores)", rom, 8, false);
        free(rom);
    }
    {
        uint32_t pat[] = { BCOND };
        rom = rom_of(4, pat, 1);
        check("conditional branch", rom, 16, false);
        free(rom);
    }
    {
        uint32_t pat[] = { BL0 };
        rom = rom_of(4, pat, 1);
        check("BL (writes LR)", rom, 16, false);
        free(rom);
    }
    {
        uint32_t pat[] = { b(-1) };
        rom = rom_of(4, pat, 1);
        check("branch before the image", rom, 16, false);
        free(rom);
    }
    {
        uint32_t pat[] = { b(4) };
        rom = rom_of(4, pat, 1);
        check("branch past the image", rom, 16, false);
        free(rom);
    }
    {
        /* Like a real ROM: branch to a reset handler that does real work. */
        rom = rom_of(1024, (uint32_t[]) { NOP }, 1);
        put(rom, 0, b(0x100));
        put(rom, 0x100, MSR_DAIFCLR);
        check("reset vector branch to real code", rom, 4096, false);
        free(rom);
    }
    {
        /* All-zero (UDF) ROM; apple_sep_check_image() rejects it first anyway. */
        rom = calloc(1024, 4);
        check("zero-filled", rom, 4096, false);
        free(rom);
    }

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: sep_rom_idle\n");
    return 0;
}
