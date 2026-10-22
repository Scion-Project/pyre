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
#include "hw/irq.h"
#include "hw/misc/a7iop/rtkit.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "agx-uat.h"
#include "trace.h"

OBJECT_DECLARE_TYPE(AppleAGXState, AppleAGXClass, APPLE_AGX)

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

/* sgx reg[1] ID block (G13 offsets relative to the 16 MiB SGX window). */
#define SGX_WINDOW_ID_BASE (0xD00000)

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

struct AppleAGXClass
{
    AppleRTKitClass  parent_class;
    ResettablePhases parent_phases;
};

struct AppleAGXState
{
    AppleRTKit parent_obj;

    MemoryRegion    asc_wrap;
    AppleAGXRegBank sgx[2];

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

static void apple_agx_reset_hold(Object* obj, ResetType type)
{
    AppleAGXState* s  = APPLE_AGX(obj);
    AppleAGXClass* ac = APPLE_AGX_GET_CLASS(obj);

    if (ac->parent_phases.hold != NULL) { ac->parent_phases.hold(obj, type); }

    s->initdata_va = 0;
    s->fw_inited   = false;
}

static void apple_agx_init(Object* obj) { }

static void apple_agx_finalize(Object* obj) { }

static void apple_agx_class_init(ObjectClass* klass, const void* data)
{
    ResettableClass* rc = RESETTABLE_CLASS(klass);
    DeviceClass*     dc = DEVICE_CLASS(klass);
    AppleAGXClass*   ac = APPLE_AGX_CLASS(klass);

    resettable_class_set_parent_phases(rc, NULL, apple_agx_reset_hold, NULL, &ac->parent_phases);
    dc->desc = "Apple AGX GPU (firmware HLE)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}
