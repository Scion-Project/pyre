/*
 * Apple T8030 SoC (iPhone 11).
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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

#pragma once

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/a13.h"
#include "hw/arm/boot.h"
#include "hw/arm/sptm-boot.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "qapi/qapi-types-usb.h"
#include "system/kvm.h"

#define TYPE_APPLE_T8030 MACHINE_TYPE_NAME("t8030")
#define TYPE_APPLE_T8101 MACHINE_TYPE_NAME("t8101")

/* Abstract parent of t8030 and t8101: one implementation, per-SoC config. */
#define TYPE_APPLE_SOC_MACHINE "apple-soc-machine"

#define APPLE_T8030(obj) OBJECT_CHECK(AppleT8030MachineState, (obj), TYPE_APPLE_SOC_MACHINE)
#define APPLE_T8030_GET_CLASS(obj) ((AppleT8030MachineClass*)MACHINE_GET_CLASS(obj))

/* One device-tree property the machine synthesizes (PMGR tables and such). */
typedef struct
{
    const char* name;
    const void* data;
    uint32_t    len;
} AppleSoCDTProp;

/*
 * Per-SoC facts the shared t8030.c implementation needs and that the device
 * tree does not carry. `t8030` and `t8101` are two instances of this table
 * (see docs/T8101-SCOPE.md section 1.1); everything else comes from the DT.
 */
typedef enum
{
    APPLE_SOC_T8030, /* A13, iPhone 11 */
    APPLE_SOC_T8101, /* A14, iPhone 12 */
} AppleSoCGen;

typedef struct
{
    AppleSoCGen gen;
    const char* name;           /* platform-name */
    const char* desc;           /* MachineClass description */
    const char* cpu_type;       /* QOM type of the application cores */
    uint32_t    chip_id;        /* /chosen/chip-id */
    uint32_t    board_id;       /* /chosen/board-id */
    uint32_t    chip_revision;  /* arm-io chip-revision and the fuse readback */
    const char* target_type;    /* root target-type */
    const char* dev_compatible; /* root compatible ("<MODEL>DEV\0<product>\0AppleARM") */
    uint32_t    dev_compatible_len;
    uint32_t    display_scale;
    uint32_t    disp_width;  /* default panel size */
    uint32_t    disp_height;
    const char* regulatory_model; /* default regulatory-model property */
    bool        real_sep;         /* real SEP (sep-rom/sep-fw) supported */

    /* PMGR: offsets inside the first `pmgr` reg range. */
    hwaddr   pmgr_cpu_start;  /* write: mask of CPUs to start */
    hwaddr   pmgr_sep_ps;     /* SEP power state */
    hwaddr   pmgr_sram_check; /* commonSramCheck read, 0 = none */
    uint32_t pmgr_sram_check_value;
    hwaddr   pmgr_ps_lo; /* power-state registers: target state mirrored */
    hwaddr   pmgr_ps_hi; /* into the actual-state field on write */
    hwaddr   pmgr_aon_ps_lo; /* absolute arm-io offsets of always-on power-state */
    hwaddr   pmgr_aon_ps_hi; /* registers in the catch-all ranges, 0 = none */
    hwaddr   pmp_base;   /* PMP catch-all inside arm-io, 0 = none */
    hwaddr   pmp_size;
    const AppleSoCDTProp* pmgr_props; /* synthesized pmgr properties */
    uint32_t              pmgr_props_count;
    const AppleSoCDTProp* armio_props; /* synthesized arm-io properties */
    uint32_t              armio_props_count;

    /* Fixed wiring with no DT description. */
    uint32_t spi0_irq;
    uint32_t gpio_spi0_cs;
    uint32_t gpio_force_dfu;
    uint32_t dwc2_irq;

    /* arm-io children this machine does not model yet; deleted before boot. */
    const char* const* rem_nodes;
    uint32_t           rem_nodes_count;
} AppleSoCConfig;

typedef struct
{
    MachineClass          parent;
    const AppleSoCConfig* soc;
} AppleT8030MachineClass;

typedef struct
{
    MachineState parent;

    const AppleSoCConfig* soc;
    hwaddr          armio_base;
    hwaddr          armio_size;
    unsigned long   dram_size;
    AppleA13State*  cpus[A13_MAX_CPU];
    AppleA13Cluster clusters[A13_MAX_CLUSTER];
    SysBusDevice*   aic;
    MachoHeader64*  kernel;
    MachoHeader64*  sptm;
    AppleDTNode*    device_tree;
    uint8_t*        trustcache;
    MachoHeader64*  txm;
    AppleSptmLayout sptm_layout;
    char*           txm_filename;
    char*           securerom;
    gsize           securerom_size;
    AppleBootInfo   boot_info;
    AppleVideoArgs  video_args;
    char*           trustcache_filename;
    char*           sep_rom_filename;
    char*           sep_fw_filename;
    char*           securerom_filename;
    char*           sptm_filename;
    uint32_t        sio_protocol;
    uint32_t        build_version;
    uint64_t        ecid;
    Notifier        init_done_notifier;
    hwaddr          panic_base;
    hwaddr          panic_size;
    uint8_t         pmgr_reg[0x100000];
    MemoryRegion    amcc;
    uint8_t         amcc_reg[0x100000];
    bool            kaslr_off;
    bool            force_dfu;
    uint32_t        board_id;
    uint32_t        chip_revision;
    char*           usb_uplink_addr;
    USBUplinkType   usb_uplink_type;
    char*           model_number;
    char*           region_info;
    char*           config_number;
    char*           serial_number;
    char*           mlb_serial_number;
    char*           regulatory_model;
    uint32_t        disp_width;
    uint32_t        disp_height;
    bool            enable_pac;
    bool            patch_check;
    bool            gpu;
} AppleT8030MachineState;
