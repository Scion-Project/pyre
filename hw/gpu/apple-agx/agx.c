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
    MemoryRegion          mr;
    uint8_t*              regs;
    uint64_t              size;
    unsigned long*        touched;    /* one bit per 32-bit register: logged once */
    hwaddr                window_off; /* offset of this region in the SGX window */
    struct AppleAGXState* agx;
    int                   index;
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

    hwaddr uat_shared;  /* kernel-half (TTBR1) L1 table */
    hwaddr uat_ttbat;   /* context TTBR table */
    hwaddr uat_handoff; /* AP/firmware page-table handoff */

    uint64_t asc_wrap_reg;
    uint64_t initdata_va;
    bool     fw_inited;
};

OBJECT_DEFINE_TYPE(AppleAGXState, apple_agx, APPLE_AGX, APPLE_RTKIT)

/* ------------------------------------------------------------------ */
/* sgx MMIO: a register file that remembers writes and logs first use. */

static uint64_t agx_sgx_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAGXRegBank* b   = opaque;
    uint64_t         val = 0;

    memcpy(&val, b->regs + addr, size);
    if (!test_and_set_bit(addr >> 2, b->touched)) { trace_apple_agx_sgx_read(b->window_off + addr, val, size); }
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

/*
 * The ASC wrapper (gfx-asc reg[1]) is one 64-bit register. RTBuddy reads
 * it, then writes the nub's firmware region base | 1 (bit 0: enable).
 */
static uint64_t agx_asc_wrap_read(void* opaque, hwaddr addr, unsigned size)
{
    AppleAGXState* s = opaque;

    trace_apple_agx_asc_wrap_read(addr, size);
    return addr == 0 ? s->asc_wrap_reg : 0;
}

static void agx_asc_wrap_write(void* opaque, hwaddr addr, uint64_t val, unsigned size)
{
    AppleAGXState* s = opaque;

    trace_apple_agx_asc_wrap_write(addr, val, size);
    if (addr == 0) { s->asc_wrap_reg = size == 8 ? val : deposit64(s->asc_wrap_reg, 0, 32, val); }
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

/*
 * Bring-up sequence observed from QEMU's side (iOS 14.8 / 18H17 research
 * build; docs/GPU-M0-NOTES.md has the traces):
 *
 *  1. The accelerator reads the sgx ID registers and pokes sgx
 *     +0xd08000..0xd08024, then waits (with a ~2.5 s timeout that ends in
 *     "Destroying invalid mutex") for the gfx-asc RTBuddy. That needs the
 *     ASC's `iommu-parent`, mapper-gfx-asc (AGXArmFirmwareMapper), to be
 *     in the device tree (hw/arm/boot.c).
 *  2. RTBuddy(GFX) resumes the "pre-loaded" firmware only if the nub has a
 *     firmware region (region-base/-size, hw/arm/t8030.c); otherwise it
 *     tries to load one and panics in _attemptFirmwareLoad. It writes
 *     region-base | 1 to the 8-byte ASC wrapper register (reg[1]).
 *  3. RTKit management handshake (HELLO, EPMAP, rollcall, power state),
 *     then the driver starts EP 0x20 and 0x21 and sends one MSG_INIT on
 *     EP 0x20 carrying the initdata pointer. That is all the mailbox
 *     traffic in a restore boot: no doorbell, no FWCtl, and every channel
 *     ring stays empty for minutes afterwards. The driver does not wait
 *     for a reply to MSG_INIT (as on G13, the firmware does not send one).
 */
/* ------------------------------------------------------------------ */
/* GPU virtual memory, as the firmware sees it through the UAT. */

/*
 * GPU VA width. Kernel (firmware-shared) objects live in the upper half:
 * MSG_INIT carries a 44-bit pointer that sign-extends to 0xffffffa0_00000000
 * and up, the same kernel IOVA base drm/asahi uses on G13.
 */
#define AGX_UAT_IAS     (39)
#define AGX_MSG_VA_BITS (44)

static bool agx_read64_pa(void* opaque, uint64_t pa, uint64_t* val)
{
    MemTxResult r = address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, val, sizeof(*val));

    *val = le64_to_cpu(*val);
    return r == MEMTX_OK;
}

static uint64_t agx_msg_va(uint64_t va) { return sextract64(va, 0, AGX_MSG_VA_BITS); }

/* Translate a GPU VA. Upper-half VAs use the shared kernel-half L1 table. */
static bool agx_translate(AppleAGXState* s, uint64_t va, uint64_t* pa)
{
    AgxUatWalk   w = {0};
    AgxUatResult r;
    uint64_t     ttb;

    if ((int64_t)va < 0) { ttb = s->uat_shared | AGX_UAT_TTBR_VALID; }
    else {
        /* Context 0's TTBR0, first entry of the TTBAT. */
        if (!agx_read64_pa(NULL, s->uat_ttbat, &ttb)) { return false; }
    }
    r = agx_uat_walk(agx_read64_pa, NULL, ttb, va, AGX_UAT_IAS, &w);
    trace_apple_agx_uat_walk(va, r, w.level, w.pte, r == AGX_UAT_OK ? w.pa : 0);
    if (r != AGX_UAT_OK) { return false; }
    *pa = w.pa;
    return true;
}

static bool agx_gpu_read(AppleAGXState* s, uint64_t va, void* buf, size_t len)
{
    uint8_t* p = buf;

    while (len > 0) {
        uint64_t pa;
        size_t   n = MIN(len, AGX_UAT_PGSZ - (va & (AGX_UAT_PGSZ - 1)));

        if (!agx_translate(s, va, &pa)) { return false; }
        if (address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, p, n) != MEMTX_OK) { return false; }
        va  += n;
        p   += n;
        len -= n;
    }
    return true;
}

/*
 * initdata, as built by the iOS 14.8 G12P driver, has the same top-level
 * layout as m1n1's G13 InitData (fw/agx/initdata.py):
 *   +0x00 regionA VA        +0x10 regionB VA     +0x18 regionC VA
 *   +0x20 fw_status VA      +0x28 u16 UAT page size (0x4000), u8 page bits
 *   (14), u8 levels (3), then three 0x20-byte level descriptors at +0x2c:
 *   u8 8, u8 14, u8 14, u8 index shift (36, 25, 14), u16 entries (8, 2048,
 *   2048), u16 table size (0x4000), u64 1, u64 PA mask (0xffffffc000),
 *   u64 index mask; +0xa0 u32 host_mapped_fw_allocations (1).
 * regionB +0x000..0x110 is 17 (state VA, ring VA) channel pairs in m1n1's
 * order (TA/3D/CL x4, DevCtrl, Event, FWLog, KTrace, Stats), then the
 * stats/hwdata pointers from +0x170 (hwdata_b repeated at +0x1a0/+0x1a8).
 * fw_status +0x00 is the FWCtl channel (state, ring) pair, pointing into
 * fw_status itself (+0x80, +0xc0). regionA's first 0x100 bytes are zero.
 */

/* Log the start of a firmware-shared structure, one 64-bit word per line. */
static void agx_dump(AppleAGXState* s, const char* what, uint64_t va, size_t len)
{
    g_autofree uint64_t* buf = g_malloc0(len);

    if (!agx_gpu_read(s, va, buf, len)) {
        trace_apple_agx_dump_fault(what, va);
        return;
    }
    for (size_t i = 0; i < len / 8; i++) {
        if (buf[i] != 0) { trace_apple_agx_dump(what, va, i * 8, le64_to_cpu(buf[i])); }
    }
}

static void agx_ep_firmware(void* opaque, uint8_t ep, uint64_t msg)
{
    AppleAGXState* s = opaque;

    trace_apple_agx_ep_msg(ep + RTKIT_EP_USER_START, msg);
    switch (AGX_MSG_TYPE(msg)) {
        case AGX_MSG_INIT:
            s->initdata_va = agx_msg_va(AGX_MSG_INIT_DATA(msg));
            s->fw_inited   = true;
            if (trace_event_get_state_backends(TRACE_APPLE_AGX_DUMP)) {
                uint64_t ttbat[4] = {0};
                address_space_read(&address_space_memory, s->uat_ttbat, MEMTXATTRS_UNSPECIFIED, ttbat, sizeof(ttbat));
                for (int i = 0; i < 4; i++) { trace_apple_agx_dump("ttbat", s->uat_ttbat, i * 8, ttbat[i]); }
                uint64_t hdr[5] = {0};
                agx_dump(s, "initdata", s->initdata_va, 0x200);
                if (agx_gpu_read(s, s->initdata_va, hdr, sizeof(hdr))) {
                    agx_dump(s, "regionA", le64_to_cpu(hdr[0]), 0x100);
                    agx_dump(s, "regionB", le64_to_cpu(hdr[2]), 0x200);
                    agx_dump(s, "regionC", le64_to_cpu(hdr[3]), 0x100);
                    agx_dump(s, "fw_status", le64_to_cpu(hdr[4]), 0x100);
                }
            }
            break;
        default: break;
    }
}

/*
 * Doorbells (EP 0x21). None are rung in a restore boot, so this only logs
 * them; for DOORBELL_DEVCTRL it also logs the DevCtrl ring's pointers (the
 * driver writes WRITE_PTR at state +0x20, the firmware READ_PTR at +0x0).
 */
#define AGX_DOORBELL_KICKFW    (0x10)
#define AGX_DOORBELL_DEVCTRL   (0x11)
#define AGX_REGIONB_CH_DEVCTRL (12)

static void agx_ep_doorbell(void* opaque, uint8_t ep, uint64_t msg)
{
    AppleAGXState* s = opaque;
    uint64_t       hdr[3];
    uint64_t       state_va;
    uint32_t       rd = 0, wr = 0;

    trace_apple_agx_ep_msg(ep + RTKIT_EP_USER_START, msg);
    if (!s->fw_inited || AGX_MSG_TYPE(msg) != AGX_MSG_TX_DOORBELL || (msg & 0xff) != AGX_DOORBELL_DEVCTRL) { return; }
    if (agx_gpu_read(s, s->initdata_va, hdr, sizeof(hdr))
        && agx_gpu_read(s, le64_to_cpu(hdr[2]) + AGX_REGIONB_CH_DEVCTRL * 16, &state_va, 8))
    {
        state_va = le64_to_cpu(state_va);
        agx_gpu_read(s, state_va, &rd, 4);
        agx_gpu_read(s, state_va + 0x20, &wr, 4);
    }
    trace_apple_agx_devctrl(le32_to_cpu(rd), le32_to_cpu(wr));
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

void apple_agx_set_uat_regions(SysBusDevice* agx, hwaddr shared, hwaddr ttbat, hwaddr handoff)
{
    AppleAGXState* s = APPLE_AGX(agx);

    s->uat_shared  = shared;
    s->uat_ttbat   = ttbat;
    s->uat_handoff = handoff;
    trace_apple_agx_uat_regions(shared, ttbat, handoff);
}

SysBusDevice* apple_agx_create(AppleDTNode* sgx, AppleDTNode* gfx_asc)
{
    DeviceState*   dev;
    AppleAGXState* s;
    AppleDTProp*   prop;
    uint64_t*      asc_reg;
    uint64_t*      sgx_reg;
    AppleDTNode*   nub;

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

    /* Like the other HLE'd IOPs: the "firmware" is already loaded and
     * running, so RTBuddy resumes it instead of loading an image. */
    nub = apple_dt_get_node(gfx_asc, "iop-gfx-nub");
    assert_nonnull(nub);
    apple_dt_set_prop_u32(nub, "pre-loaded", 1);
    apple_dt_set_prop_u32(nub, "running", 1);

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
    AppleAGXState*      s  = APPLE_AGX(obj);
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

    s->asc_wrap_reg = 0;
    s->initdata_va  = 0;
    s->fw_inited    = false;
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
    ResettableClass*    rc = RESETTABLE_CLASS(klass);
    DeviceClass*        dc = DEVICE_CLASS(klass);
    AppleAGXStateClass* ac = APPLE_AGX_CLASS(klass);

    resettable_class_set_parent_phases(rc, NULL, apple_agx_reset_hold, NULL, &ac->parent_phases);
    dc->desc = "Apple AGX GPU (firmware HLE)";
    device_class_set_props(dc, apple_agx_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}
