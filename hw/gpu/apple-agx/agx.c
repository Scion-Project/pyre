/*
 * Apple AGX GPU (A13 "G12P"): sgx register stubs + gfx-asc firmware HLE.
 *
 * Milestone M0 of docs/GPU-EMULATION-SCOPE.md: the guest's own AGX kexts
 * load, the RTKit handshake with the GPU coprocessor (gfx-asc) completes,
 * and work completes as a no-op. QEMU plays the GPU firmware's role (as it
 * does for AOP, SMC, SIO and ANS); no firmware ever runs.
 *
 * Only created with `-M t8030,gpu=on`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpu/apple-agx.h"
#include "hw/misc/a7iop/rtkit.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "system/address-spaces.h"
#include "agx-uat.h"
#include "trace.h"

OBJECT_DECLARE_TYPE(AppleAGXState, AppleAGXStateClass, APPLE_AGX)

/* RTKit user endpoints (EP 0x20/0x21), as on G13 (drm/asahi gpu.rs). */
#define AGX_EP_FIRMWARE (0x20 - RTKIT_EP_USER_START)
#define AGX_EP_DOORBELL (0x21 - RTKIT_EP_USER_START)

#define AGX_MSG_TYPE(_m)      ((_m) >> 48)
#define AGX_MSG_INIT          (0x81)
#define AGX_MSG_INIT_DATA(_m) ((_m) & ((1ULL << 44) - 1))
#define AGX_MSG_TX_DOORBELL   (0x83)
#define AGX_MSG_FWCTL         (0x84)
#define AGX_MSG_HALT          (0x85)
#define AGX_MSG_RX_DOORBELL   (0x42ULL << 48)

/*
 * GPU identification registers (offsets in the 16 MiB SGX window; the same
 * addresses as G13's in drm/asahi regs.rs, but the G12 kext decodes them
 * differently). The accelerator's probe() reads all six:
 *   ID_VERSION [31:16] must be 0x0300 (G12); [15:12] major revision
 *     (0 = A, 1 = B), [11:8] minor: A0 and B0 are the two personalities.
 *   ID_COUNTS_1 [7:0], [15:8], [23:16]; ID_COUNTS_2 [7:0], [15:8], [23:16];
 *   ID_CLUSTERS [19:12] and four 4-bit fields multiplied with ID_UNK18's
 *   [19:16], [23:20], [27:24] into per-cluster unit counts.
 */
#define SGX_ID_VERSION  (0xD04000)
#define SGX_ID_UNK08    (0xD04008)
#define SGX_ID_COUNTS_1 (0xD04010) /* [7:0] cores per cluster */
#define SGX_ID_COUNTS_2 (0xD04014) /* [23:16] GPs */
#define SGX_ID_UNK18    (0xD04018)
#define SGX_ID_CLUSTERS (0xD0401C) /* [19:12] clusters */
#define SGX_CORE_MASK_0 (0xD01500)
#define SGX_CORE_MASK_1 (0xD01514)

enum
{
    AGX_MMIO_ASC = 0,
    AGX_MMIO_ASC_WRAP,
    AGX_MMIO_SGX0,
    AGX_MMIO_SGX1,
    AGX_MMIO_COUNT,
};

typedef struct
{
    MemoryRegion mr;
    uint8_t*     regs;
    uint64_t     size;
    unsigned long* touched; /* one bit per 32-bit register: logged once */
    hwaddr       window_off; /* offset of this region in the SGX window */
    struct AppleAGXState* agx;
    int          index;
} AppleAGXRegBank;

struct AppleAGXStateClass
{
    AppleRTKitClass  parent_class;
    ResettablePhases parent_phases;
};

struct AppleAGXState
{
    AppleRTKit parent_obj;

    MemoryRegion    asc_wrap;
    AppleAGXRegBank sgx[2];

    /* Reset values of the ID registers (qdev properties, so they can be
     * overridden with -global apple-agx.<name>=<value>). */
    uint32_t id_version;
    uint32_t id_unk08;
    uint32_t id_counts_1;
    uint32_t id_counts_2;
    uint32_t id_unk18;
    uint32_t id_clusters;
    uint32_t core_mask_0;
    uint32_t core_mask_1;

    uint64_t initdata_va;
    bool     fw_inited;
};

OBJECT_DEFINE_TYPE(AppleAGXState, apple_agx, APPLE_AGX, APPLE_RTKIT)

/* ------------------------------------------------------------------ */
/* sgx MMIO: a register file that remembers writes and logs first use. */

static uint64_t agx_sgx_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAGXRegBank* b = opaque;
    uint64_t         val = 0;

    memcpy(&val, b->regs + addr, size);
    if (!test_and_set_bit(addr >> 2, b->touched)) {
        trace_apple_agx_sgx_read(b->window_off + addr, val, size);
    }
    return val;
}

static void agx_sgx_write(void* opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AppleAGXRegBank* b = opaque;

    trace_apple_agx_sgx_write(b->window_off + addr, val, size);
    memcpy(b->regs + addr, &val, size);
}

static const MemoryRegionOps agx_sgx_ops = {
    .read                  = agx_sgx_read,
    .write                 = agx_sgx_write,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
};

static uint64_t agx_asc_wrap_read(void* opaque, hwaddr addr, unsigned size)
{
    trace_apple_agx_asc_wrap_read(addr, size);
    return 0;
}

static void agx_asc_wrap_write(void* opaque, hwaddr addr, uint64_t val, unsigned size)
{
    trace_apple_agx_asc_wrap_write(addr, val, size);
}

static const MemoryRegionOps agx_asc_wrap_ops = {
    .read                  = agx_asc_wrap_read,
    .write                 = agx_asc_wrap_write,
    .endianness            = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size  = 4,
    .impl.max_access_size  = 8,
};

/* ------------------------------------------------------------------ */
/* Firmware endpoints. */

static void agx_ep_firmware(void* opaque, uint8_t ep, uint64_t msg)
{
    AppleAGXState* s = opaque;

    trace_apple_agx_ep_msg(ep + RTKIT_EP_USER_START, msg);
    switch (AGX_MSG_TYPE(msg)) {
        case AGX_MSG_INIT:
            s->initdata_va = AGX_MSG_INIT_DATA(msg);
            s->fw_inited   = true;
            break;
        default: break;
    }
}

static void agx_ep_doorbell(void* opaque, uint8_t ep, uint64_t msg)
{
    trace_apple_agx_ep_msg(ep + RTKIT_EP_USER_START, msg);
}

static void agx_boot_done(void* opaque) { trace_apple_agx_boot_done(); }

static const AppleRTKitOps agx_rtkit_ops = {
    .boot_done = agx_boot_done,
};

/* ------------------------------------------------------------------ */

static void agx_init_bank(AppleAGXState* s, int i, uint64_t window_off, uint64_t size)
{
    AppleAGXRegBank* b = &s->sgx[i];
    char             name[32];

    b->agx        = s;
    b->index      = i;
    b->size       = size;
    b->window_off = window_off;
    b->regs       = g_malloc0(size);
    b->touched    = bitmap_new(size >> 2);
    snprintf(name, sizeof(name), TYPE_APPLE_AGX ".sgx%d", i);
    memory_region_init_io(&b->mr, OBJECT(s), &agx_sgx_ops, b, name, size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &b->mr);
}

SysBusDevice* apple_agx_create(AppleDTNode* sgx, AppleDTNode* gfx_asc)
{
    DeviceState*   dev;
    AppleAGXState* s;
    AppleDTProp*   prop;
    uint64_t*      asc_reg;
    uint64_t*      sgx_reg;

    dev = qdev_new(TYPE_APPLE_AGX);
    s   = APPLE_AGX(dev);

    prop = apple_dt_get_prop(gfx_asc, "reg");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, >=, 32);
    asc_reg = (uint64_t*)prop->data;

    prop = apple_dt_get_prop(sgx, "reg");
    assert_nonnull(prop);
    assert_cmpuint(prop->len, >=, 32);
    sgx_reg = (uint64_t*)prop->data;

    apple_rtkit_init(APPLE_RTKIT(dev), s, "GFX", asc_reg[1], APPLE_A7IOP_V4, &agx_rtkit_ops);
    apple_rtkit_register_user_ep(APPLE_RTKIT(dev), AGX_EP_FIRMWARE, s, agx_ep_firmware);
    apple_rtkit_register_user_ep(APPLE_RTKIT(dev), AGX_EP_DOORBELL, s, agx_ep_doorbell);

    memory_region_init_io(&s->asc_wrap, OBJECT(dev), &agx_asc_wrap_ops, s, TYPE_APPLE_AGX ".asc-wrap", asc_reg[3]);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->asc_wrap);

    /* The two sgx ranges are windows into one 16 MiB register space; keep
     * offsets relative to the first so they read like drm/asahi's. */
    agx_init_bank(s, 0, 0, sgx_reg[1]);
    agx_init_bank(s, 1, sgx_reg[2] - sgx_reg[0], sgx_reg[3]);

    return SYS_BUS_DEVICE(dev);
}

/* Store a reset value into whichever bank covers SGX window offset `off`. */
static void agx_sgx_preset(AppleAGXState* s, hwaddr off, uint32_t val)
{
    for (size_t i = 0; i < ARRAY_SIZE(s->sgx); i++) {
        AppleAGXRegBank* b = &s->sgx[i];
        if (off >= b->window_off && off + 4 <= b->window_off + b->size) {
            memcpy(b->regs + (off - b->window_off), &val, 4);
            return;
        }
    }
    g_assert_not_reached();
}

static void apple_agx_reset_hold(Object* obj, ResetType type)
{
    AppleAGXState* s  = APPLE_AGX(obj);
    AppleAGXStateClass* ac = APPLE_AGX_GET_CLASS(obj);

    if (ac->parent_phases.hold != NULL) { ac->parent_phases.hold(obj, type); }

    for (size_t i = 0; i < ARRAY_SIZE(s->sgx); i++) {
        memset(s->sgx[i].regs, 0, s->sgx[i].size);
        bitmap_zero(s->sgx[i].touched, s->sgx[i].size >> 2);
    }
    agx_sgx_preset(s, SGX_ID_VERSION, s->id_version);
    agx_sgx_preset(s, SGX_ID_UNK08, s->id_unk08);
    agx_sgx_preset(s, SGX_ID_COUNTS_1, s->id_counts_1);
    agx_sgx_preset(s, SGX_ID_COUNTS_2, s->id_counts_2);
    agx_sgx_preset(s, SGX_ID_UNK18, s->id_unk18);
    agx_sgx_preset(s, SGX_ID_CLUSTERS, s->id_clusters);
    agx_sgx_preset(s, SGX_CORE_MASK_0, s->core_mask_0);
    agx_sgx_preset(s, SGX_CORE_MASK_1, s->core_mask_1);

    s->initdata_va = 0;
    s->fw_inited   = false;
}

/*
 * A13 ("G12P"), revision B0 (the AGXAcceleratorG12P_B0 personality, whose
 * Metal plugin is AGXMetalA13): one cluster of four cores.
 */
static const Property apple_agx_properties[] = {
    DEFINE_PROP_UINT32("id-version", AppleAGXState, id_version, 0x03001000),
    DEFINE_PROP_UINT32("id-unk08", AppleAGXState, id_unk08, 0),
    DEFINE_PROP_UINT32("id-counts-1", AppleAGXState, id_counts_1, 0x00010104),
    DEFINE_PROP_UINT32("id-counts-2", AppleAGXState, id_counts_2, 0x00010000),
    DEFINE_PROP_UINT32("id-unk18", AppleAGXState, id_unk18, 0),
    DEFINE_PROP_UINT32("id-clusters", AppleAGXState, id_clusters, 0x00001000),
    DEFINE_PROP_UINT32("core-mask-0", AppleAGXState, core_mask_0, 0xF),
    DEFINE_PROP_UINT32("core-mask-1", AppleAGXState, core_mask_1, 0),
};

static void apple_agx_init(Object* obj) { }

static void apple_agx_finalize(Object* obj) { }

static void apple_agx_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    AppleAGXStateClass*   ac = APPLE_AGX_CLASS(klass);

    resettable_class_set_parent_phases(rc, NULL, apple_agx_reset_hold, NULL, &ac->parent_phases);
    dc->desc = "Apple AGX GPU (firmware HLE)";
    device_class_set_props(dc, apple_agx_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}
