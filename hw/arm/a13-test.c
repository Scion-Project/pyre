/*
 * Apple A13 CPU test bench ("a13-test" machine).
 *
 * One AppleA13 core (the same CPU type the t8030 machine creates, so it has
 * the full A13 + GXF/SPTM system-register set), RAM, and a test-control
 * register block. No SoC devices, no device tree and no firmware of any kind.
 *
 * It exists so bare-metal CPU-model tests (hw/arm/tests/) can run on a plain
 * host build and report a result through QEMU's exit status:
 *
 *   qemu-system-aarch64 -M a13-test -display none -kernel test.bin
 *
 * The raw image is copied to A13_TEST_RAM_BASE and the core starts there at
 * EL1 (AArch64, MMU off), the way t8030 hands over to a kernel or SPTM.
 *
 * Test-control block at A13_TEST_CTL_BASE (64-bit little-endian registers):
 *   +0x00 DETAIL  scratch value printed with the result (e.g. an ESR)
 *   +0x08 STATUS  write A13_TEST_PASS to exit 0; any other value exits 1
 *   +0x10 PUTC    low byte is written to stderr (debug output)
 *
 * This machine is only used by tests. Nothing else instantiates it, so it
 * has no effect on t8030 or s8000 boots.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/a13.h"
#include "hw/boards.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "arm-powerctl.h"

#define TYPE_A13_TEST_MACHINE MACHINE_TYPE_NAME("a13-test")
OBJECT_DECLARE_SIMPLE_TYPE(A13TestMachineState, A13_TEST_MACHINE)

#define A13_TEST_RAM_BASE (0x800000000ULL)
#define A13_TEST_CTL_BASE (0x200000000ULL)
#define A13_TEST_CTL_SIZE (0x1000)

#define A13_TEST_REG_DETAIL (0x00)
#define A13_TEST_REG_STATUS (0x08)
#define A13_TEST_REG_PUTC   (0x10)

#define A13_TEST_PASS (0x50415353ULL) /* "PASS" */

struct A13TestMachineState
{
    MachineState parent_obj;

    AppleA13State* cpu;
    MemoryRegion   ctl;
    gchar*         image;
    gsize          image_size;
    uint64_t       detail;
};

static uint64_t a13_test_ctl_read(void* opaque, hwaddr addr, unsigned size)
{
    A13TestMachineState* s = opaque;

    return addr == A13_TEST_REG_DETAIL ? s->detail : 0;
}

static void a13_test_ctl_write(void* opaque, hwaddr addr, uint64_t value, unsigned size)
{
    A13TestMachineState* s = opaque;

    switch (addr) {
        case A13_TEST_REG_DETAIL: s->detail = value; break;
        case A13_TEST_REG_STATUS:
            if (value == A13_TEST_PASS) {
                info_report("a13-test: PASS");
                qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN, 0);
            }
            else {
                error_report("a13-test: FAIL status=0x%" PRIx64 " detail=0x%" PRIx64, value, s->detail);
                qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN, 1);
            }
            break;
        case A13_TEST_REG_PUTC: fputc((int)(value & 0xff), stderr); break;
        default: break;
    }
}

static const MemoryRegionOps a13_test_ctl_ops = {
    .read       = a13_test_ctl_read,
    .write      = a13_test_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = {.min_access_size = 1, .max_access_size = 8},
    .impl       = {.min_access_size = 8, .max_access_size = 8},
};

static void a13_test_init(MachineState* machine)
{
    A13TestMachineState* s = A13_TEST_MACHINE(machine);

    if (machine->kernel_filename == NULL) {
        error_setg(&error_fatal, "a13-test: pass the bare-metal test image with -kernel");
        return;
    }
    if (!g_file_get_contents(machine->kernel_filename, &s->image, &s->image_size, NULL)) {
        error_setg(&error_fatal, "a13-test: could not read `%s`", machine->kernel_filename);
        return;
    }
    if (s->image_size == 0 || s->image_size > machine->ram_size) {
        error_setg(&error_fatal, "a13-test: `%s` is empty or larger than RAM", machine->kernel_filename);
        return;
    }

    memory_region_add_subregion(get_system_memory(), A13_TEST_RAM_BASE, machine->ram);

    memory_region_init_io(&s->ctl, OBJECT(machine), &a13_test_ctl_ops, s, "a13-test.ctl", A13_TEST_CTL_SIZE);
    memory_region_add_subregion(get_system_memory(), A13_TEST_CTL_BASE, &s->ctl);

    s->cpu = apple_a13_create("cpu0", 0, 0, 0, 'P');
    object_property_add_child(OBJECT(machine), "cpu0", OBJECT(s->cpu));
    qdev_realize(DEVICE(s->cpu), NULL, &error_fatal);
}

static void a13_test_reset(MachineState* machine, ResetType type)
{
    A13TestMachineState* s   = A13_TEST_MACHINE(machine);
    CPUState*            cpu = CPU(s->cpu);

    qemu_devices_reset(type);

    s->detail = 0;
    address_space_write(&address_space_memory, A13_TEST_RAM_BASE, MEMTXATTRS_UNSPECIFIED, s->image,
                        s->image_size);

    object_property_set_uint(OBJECT(cpu), "rvbar", A13_TEST_RAM_BASE, &error_abort);
    cpu_reset(cpu);
    arm_set_cpu_on(s->cpu->parent_obj.mp_affinity, A13_TEST_RAM_BASE, 0, 1, true);
}

static void a13_test_class_init(ObjectClass* klass, const void* data)
{
    MachineClass* mc = MACHINE_CLASS(klass);

    mc->desc              = "Apple A13 CPU test bench (bare-metal CPU-model tests, no firmware)";
    mc->init              = a13_test_init;
    mc->reset             = a13_test_reset;
    mc->max_cpus          = 1;
    mc->default_cpu_type  = TYPE_APPLE_A13;
    mc->minimum_page_bits = 14;
    mc->default_ram_size  = 64 * MiB;
    mc->default_ram_id    = "a13-test.ram";
}

static const TypeInfo a13_test_info = {
    .name   = TYPE_A13_TEST_MACHINE,
    .parent = TYPE_MACHINE,
    OBJECT_TYPE_INSTANCE(A13TestMachineState),
    .class_init = a13_test_class_init,
};

static void a13_test_types(void) { type_register_static(&a13_test_info); }

type_init(a13_test_types)
