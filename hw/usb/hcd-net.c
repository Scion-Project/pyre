/*
 * USB uplink that acts as the host for the emulated device's CDC-NCM function
 * and bridges it to a QEMU network backend (usb-uplink-type=net).
 *
 * An iPhone's USB device stack can expose an Ethernet function: a host that
 * sends Apple's vendor SET_MODE request (0x52, wIndex 3, as usbmuxd does with
 * USBMUXD_DEFAULT_DEVICE_MODE=3) makes the device re-enumerate with a fifth
 * configuration that carries a CDC-NCM interface pair next to usbmux. iOS then
 * runs DHCP on that link and uses it as a route, the same as a Mac sharing its
 * connection to an iPhone over USB. This device is that host: it enumerates
 * the guest's device, switches the mode, selects the NCM configuration and
 * moves Ethernet frames between NTB16 transfers and a -netdev (for example
 * -netdev user, i.e. slirp).
 *
 * Everything runs in coroutines on the main loop. A session starts when the
 * device attaches and stops as soon as it detaches (s->generation moves), so a
 * re-enumeration simply starts the next session.
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
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/hcd-net.h"
#include "hw/usb/usb-uplink.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/coroutine.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

// #define DEBUG_USB_NET_UPLINK

#ifdef DEBUG_USB_NET_UPLINK
    #define DPRINTF(fmt, ...)                                       \
        do {                                                        \
            fprintf(stderr, "usb-net-uplink: " fmt, ##__VA_ARGS__); \
        }                                                           \
        while (0)
#else
    #define DPRINTF(fmt, ...) \
        do { }                \
        while (0)
#endif

#define APPLE_VENDOR_ID         0x05AC
#define APPLE_REQ_GET_MODE      0x45
#define APPLE_REQ_SET_MODE      0x52
#define APPLE_MODE_CDC_NCM      3
#define APPLE_NCM_CONFIGURATION 5
#define MAX_MODE_SWITCHES       2

#define USB_CDC_SUBCLASS_NCM   0x0D
#define USB_CDC_ETHERNET_TYPE  0x0F
#define USB_CDC_NCM_NTB16      0x0000
#define USB_CDC_NCM_NOTIFY_NET 0x00
#define USB_CDC_NCM_NOTIFY_SPD 0x2A

#define NCM_REQ_GET_NTB_PARAMETERS 0x80
#define NCM_REQ_SET_NTB_FORMAT     0x84
#define NCM_REQ_SET_NTB_INPUT_SIZE 0x86
#define NCM_NTB_PARAMETERS_LEN     28

#define NCM_NTH16_SIGN   0x484D434E /* "NCMH" */
#define NCM_NDP16_NOCRC  0x304D434E /* "NCM0" */
#define NCM_NDP16_CRC    0x314D434E /* "NCM1" */
#define NCM_NTH16_LEN    12
#define NCM_NDP16_HDRLEN 8

/* What we ask the device to send at most per NTB, and the cap on what we queue. */
#define NCM_NTB_IN_SIZE_WANTED 16384u
#define NCM_TX_QUEUE_MAX_BYTES (256 * 1024)
#define ETH_FRAME_MAX          1514
#define ETH_FRAME_MIN          14
#define NCM_TX_BUF_SIZE        8192

/* Give the guest a moment between attach and the first request, like a hub's debounce. */
#define USB_NET_SETTLE_MS 100
#define USB_NET_HS_MPS    512

/* While an endpoint NAKs: poll every 1 ms for the first second, then every 10 ms. */
#define USB_NET_POLL_NS      (1 * SCALE_MS)
#define USB_NET_IDLE_POLL_NS (10 * SCALE_MS)

struct USBNetSession
{
    USBNetUplinkState* s;
    uint32_t           generation;
    /* Set when the device goes away, so polling loops in the uplink helpers stop. */
    bool stale;
    QLIST_ENTRY(USBNetSession) link;
};

static USBNetSession* usb_net_session_new(USBNetUplinkState* s)
{
    USBNetSession* sess = g_new0(USBNetSession, 1);

    sess->s          = s;
    sess->generation = s->generation;
    QLIST_INSERT_HEAD(&s->sessions, sess, link);
    return sess;
}

static void usb_net_session_free(USBNetSession* sess)
{
    QLIST_REMOVE(sess, link);
    g_free(sess);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(USBNetSession, usb_net_session_free)

static void usb_net_sessions_mark_stale(USBNetUplinkState* s)
{
    USBNetSession* sess;

    QLIST_FOREACH (sess, &s->sessions, link) { sess->stale = true; }
}

static void usb_net_uplink_free_frame(gpointer data) { g_byte_array_free(data, TRUE); }

static bool usb_net_session_alive(USBNetUplinkState* s, uint32_t generation)
{
    return s->generation == generation && usb_uplink_active(&s->usb) != NULL;
}

/*
 * Submit one transfer on a data endpoint and wait for it: polled while the
 * device NAKs, parked on the packet if it completes later. Returns a
 * USB_RET_* status.
 */
static int32_t coroutine_fn usb_net_xfer(USBNetSession* sess, USBUplinkPacket* pkt, bool* busy, uint8_t ep_addr,
                                         void* buf, uint32_t len, uint32_t* actual)
{
    USBNetUplinkState* s          = sess->s;
    uint32_t           generation = sess->generation;
    int64_t            started    = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int32_t            status;

    *actual = 0;
    if (sess->stale || !usb_net_session_alive(s, generation)) { return USB_RET_NODEV; }

    usb_packet_init(&pkt->p);
    *busy = true;

    /*
     * Not usb_uplink_transfer(): an idle link NAKs for as long as nothing is
     * sent, which is normal here and shouldn't be warned about.
     */
    for (;;) {
        int64_t waited;

        status = usb_uplink_submit(usb_uplink_active(&s->usb), pkt, ep_addr, buf, len);
        if ((status != USB_RET_NAK && status != USB_RET_IOERROR) || sess->stale) { break; }

        usb_uplink_packet_reset(pkt);
        waited = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - started;
        qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, waited > NANOSECONDS_PER_SECOND ? USB_NET_IDLE_POLL_NS : USB_NET_POLL_NS);
    }

    if (status == USB_RET_ASYNC) {
        pkt->waiter = qemu_coroutine_self();
        qemu_coroutine_yield();
        status = pkt->p.status;
    }

    *actual = pkt->p.actual_length;
    *busy   = false;
    usb_packet_cleanup(&pkt->p);

    if (!usb_net_session_alive(s, generation)) { return USB_RET_NODEV; }
    return status;
}

static int32_t coroutine_fn usb_net_control(USBNetUplinkState* s, USBDevice* dev, uint8_t type, uint8_t req,
                                            uint16_t value, uint16_t index, uint16_t length, void* buf,
                                            uint32_t* actual)
{
    int32_t status;

    WITH_QEMU_LOCK_GUARD(&s->usb.ctrl_lock)
    {
        status = usb_uplink_control(dev, type, req, value, index, length, buf, actual, true);
    }

    DPRINTF("control %02x %02x %04x %04x %04x -> %d (%u)\n", type, req, value, index, length, status,
            actual ? *actual : 0);
    return status;
}

/* Finds the configuration whose bConfigurationValue is @value. */
static const uint8_t* usb_net_find_config(const USBUplinkDescriptors* desc, uint8_t value, uint32_t* out_len)
{
    uint32_t off = 0;

    while (off + 9 <= desc->configs_len) {
        const uint8_t* c     = desc->configs + off;
        uint32_t       total = lduw_le_p(c + 2);

        if (c[1] != USB_DT_CONFIG || total < 9 || off + total > desc->configs_len) { return NULL; }
        if (c[5] == value) {
            *out_len = total;
            return c;
        }
        off += total;
    }

    return NULL;
}

/*
 * Walks one configuration for a CDC-NCM control interface, its data interface
 * and the data interface's alternate setting with two bulk endpoints.
 */
static bool usb_net_parse_ncm(const uint8_t* cfg, uint32_t len, USBNetUplinkNCM* ncm)
{
    uint32_t off          = cfg[0];
    int      cur_iface    = -1;
    int      cur_alt      = 0;
    int      cur_class    = -1;
    int      cur_subclass = -1;
    bool     have_ctrl    = false;
    bool     have_data    = false;
    int      pending_in   = -1;
    int      pending_out  = -1;
    int      union_slave  = -1;
    uint8_t  intr_ep      = 0;

    memset(ncm, 0, sizeof(*ncm));
    ncm->config_value = cfg[5];

    while (off + 2 <= len) {
        const uint8_t* d     = cfg + off;
        uint8_t        dlen  = d[0];
        uint8_t        dtype = d[1];

        if (dlen < 2 || off + dlen > len) { break; }

        switch (dtype) {
            case USB_DT_INTERFACE:
                if (dlen < 9) { break; }
                cur_iface    = d[2];
                cur_alt      = d[3];
                cur_class    = d[5];
                cur_subclass = d[6];
                pending_in = pending_out = -1;

                if (cur_class == USB_CLASS_COMM && cur_subclass == USB_CDC_SUBCLASS_NCM) {
                    ncm->ctrl_iface = cur_iface;
                    have_ctrl       = true;
                }
                break;
            case USB_DT_CS_INTERFACE:
                if (cur_class != USB_CLASS_COMM || cur_subclass != USB_CDC_SUBCLASS_NCM || dlen < 3) { break; }
                if (d[2] == 0x06 && dlen >= 5) { union_slave = d[4]; }
                if (d[2] == USB_CDC_ETHERNET_TYPE && dlen >= 13) {
                    ncm->mac_string  = d[3];
                    ncm->max_segment = lduw_le_p(d + 8);
                }
                break;
            case USB_DT_ENDPOINT:
                if (dlen < 7) { break; }
                if (have_ctrl && cur_iface == ncm->ctrl_iface && (d[3] & 3) == 3 && (d[2] & USB_DIR_IN)) {
                    intr_ep = d[2];
                }
                if (cur_class == USB_CLASS_CDC_DATA && (d[3] & 3) == 2) {
                    if (d[2] & USB_DIR_IN) { pending_in = d[2]; }
                    else {
                        pending_out = d[2];
                    }
                    if (pending_in >= 0 && pending_out >= 0 && !have_data &&
                        (union_slave < 0 || union_slave == cur_iface)) {
                        ncm->data_iface = cur_iface;
                        ncm->data_alt   = cur_alt;
                        ncm->ep_in      = pending_in;
                        ncm->ep_out     = pending_out;
                        have_data       = true;
                    }
                }
                break;
            default: break;
        }

        off += dlen;
    }

    ncm->ep_intr = intr_ep;
    return have_ctrl && have_data;
}

static void usb_net_set_link(USBNetUplinkState* s, bool up)
{
    NetClientState* nc = qemu_get_queue(s->nic);

    if (s->link_up == up) { return; }

    s->link_up = up;
    if (up) { qemu_flush_queued_packets(nc); }
    else {
        g_queue_clear_full(&s->tx_queue, usb_net_uplink_free_frame);
        s->tx_queue_bytes = 0;
    }
}

/* Unpacks every datagram in one NTB16 and hands it to the backend. */
static void usb_net_rx_ntb(USBNetUplinkState* s, const uint8_t* buf, uint32_t len)
{
    NetClientState* nc = qemu_get_queue(s->nic);
    uint32_t        ndp;
    uint32_t        block_len;
    int             guard = 0;

    if (len < NCM_NTH16_LEN || ldl_le_p(buf) != NCM_NTH16_SIGN || lduw_le_p(buf + 4) != NCM_NTH16_LEN) {
        DPRINTF("rx: not an NTB16 (%u bytes)\n", len);
        return;
    }

    block_len = lduw_le_p(buf + 8);
    if (block_len != 0 && block_len < len) { len = block_len; }
    ndp = lduw_le_p(buf + 10);

    while (ndp != 0 && guard++ < 32) {
        uint32_t sig, ndp_len, i;

        if (ndp + NCM_NDP16_HDRLEN > len) { return; }

        sig     = ldl_le_p(buf + ndp);
        ndp_len = lduw_le_p(buf + ndp + 4);
        if ((sig != NCM_NDP16_NOCRC && sig != NCM_NDP16_CRC) || ndp_len < 16 || ndp + ndp_len > len) { return; }

        for (i = NCM_NDP16_HDRLEN; i + 4 <= ndp_len; i += 4) {
            uint32_t index = lduw_le_p(buf + ndp + i);
            uint32_t dlen  = lduw_le_p(buf + ndp + i + 2);

            if (index == 0 || dlen == 0) { break; }
            if (sig == NCM_NDP16_CRC) { dlen = dlen >= 4 ? dlen - 4 : 0; }
            if (dlen < ETH_FRAME_MIN || index + dlen > len) { continue; }

            if (s->rx_frames++ == 0) {
                info_report("usb-net-uplink: first frame from the guest (%u bytes, ethertype 0x%04x)", dlen,
                            lduw_be_p(buf + index + 12));
            }
            qemu_send_packet(nc, buf + index, dlen);
        }

        ndp = lduw_le_p(buf + ndp + 6);
    }
}

static void coroutine_fn usb_net_rx_co(void* opaque)
{
    g_autoptr(USBNetSession) sess = opaque;
    USBNetUplinkState*       s    = sess->s;
    g_autofree uint8_t*      buf  = g_malloc(s->ncm.ntb_in_size);

    while (usb_net_session_alive(s, sess->generation)) {
        uint32_t actual = 0;
        int32_t  status =
            usb_net_xfer(sess, &s->rx_pkt, &s->rx_busy, s->ncm.ep_in, buf, s->ncm.ntb_in_size, &actual);

        if (status == USB_RET_NODEV) { break; }
        if (status != USB_RET_SUCCESS) {
            DPRINTF("rx: status %d\n", status);
            qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, 10 * SCALE_MS);
            continue;
        }

        usb_net_rx_ntb(s, buf, actual);
    }

    DPRINTF("rx: session %u over\n", sess->generation);
}

static void coroutine_fn usb_net_intr_co(void* opaque)
{
    g_autoptr(USBNetSession) sess = opaque;
    USBNetUplinkState*       s    = sess->s;
    uint8_t                  buf[16];

    while (usb_net_session_alive(s, sess->generation)) {
        uint32_t actual = 0;
        int32_t  status =
            usb_net_xfer(sess, &s->intr_pkt, &s->intr_busy, s->ncm.ep_intr, buf, sizeof(buf), &actual);

        if (status == USB_RET_NODEV) { break; }
        if (status != USB_RET_SUCCESS || actual < 8) {
            qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, 100 * SCALE_MS);
            continue;
        }

        if (buf[1] == USB_CDC_NCM_NOTIFY_NET) {
            info_report("usb-net-uplink: guest reports network %s", lduw_le_p(buf + 2) ? "connected" : "disconnected");
        }
        else if (buf[1] == USB_CDC_NCM_NOTIFY_SPD && actual >= 16) {
            DPRINTF("speed change: down %u up %u\n", ldl_le_p(buf + 8), ldl_le_p(buf + 12));
        }
    }
}

/* Wraps one Ethernet frame in an NTB16 with a single NDP after the datagram. */
static uint32_t usb_net_build_ntb(USBNetUplinkState* s, uint8_t* out, uint32_t out_size, const uint8_t* frame,
                                  uint32_t len)
{
    uint32_t divisor   = s->ncm.ndp_out_divisor ?: 4;
    uint32_t remainder = s->ncm.ndp_out_remainder % divisor;
    uint32_t align     = s->ncm.ndp_out_alignment ?: 4;
    uint32_t data, ndp, total;

    /* Datagram start: NTH16, then pad to satisfy wNdpOutDivisor/wNdpOutPayloadRemainder. */
    data = NCM_NTH16_LEN;
    data += (divisor + remainder - (data % divisor)) % divisor;

    ndp   = ROUND_UP(data + len, align);
    total = ndp + 16; /* NDP16 header plus one entry plus the terminating zero entry */
    if (total + 1 > out_size) { return UINT32_MAX; }

    memset(out, 0, total + 1);

    stl_le_p(out, NCM_NTH16_SIGN);
    stw_le_p(out + 4, NCM_NTH16_LEN);
    stw_le_p(out + 6, s->tx_seq++);
    stw_le_p(out + 10, ndp);

    memcpy(out + data, frame, len);

    stl_le_p(out + ndp, NCM_NDP16_NOCRC);
    stw_le_p(out + ndp + 4, 16);
    stw_le_p(out + ndp + 6, 0);
    stw_le_p(out + ndp + 8, data);
    stw_le_p(out + ndp + 10, len);

    /* NCM devices may not expect a zero-length packet: never end on a packet boundary. */
    if (total % USB_NET_HS_MPS == 0) { total++; }

    stw_le_p(out + 8, total);
    return total;
}

static void coroutine_fn usb_net_tx_co(void* opaque)
{
    g_autoptr(USBNetSession) sess = opaque;
    USBNetUplinkState*       s    = sess->s;
    g_autofree uint8_t*      ntb  = g_malloc(NCM_TX_BUF_SIZE);

    while (usb_net_session_alive(s, sess->generation) && !g_queue_is_empty(&s->tx_queue)) {
        GByteArray* frame = g_queue_pop_head(&s->tx_queue);
        uint32_t    actual;
        uint32_t    len;

        s->tx_queue_bytes -= frame->len;
        len = usb_net_build_ntb(s, ntb, NCM_TX_BUF_SIZE, frame->data, frame->len);
        g_byte_array_free(frame, TRUE);

        if (len == UINT32_MAX || (s->ncm.ntb_out_max != 0 && len > s->ncm.ntb_out_max)) {
            DPRINTF("tx: NTB of %u bytes exceeds the device's %u\n", len, s->ncm.ntb_out_max);
            continue;
        }

        if (usb_net_xfer(sess, &s->tx_pkt, &s->tx_busy, s->ncm.ep_out, ntb, len, &actual) == USB_RET_NODEV) { break; }
    }

    s->tx_running = false;
    if (usb_net_session_alive(s, sess->generation)) { qemu_flush_queued_packets(qemu_get_queue(s->nic)); }
}

static void usb_net_kick_tx(USBNetUplinkState* s)
{
    USBNetSession* sess;

    if (s->tx_running || !s->link_up || g_queue_is_empty(&s->tx_queue)) { return; }

    sess          = usb_net_session_new(s);
    s->tx_running = true;
    qemu_coroutine_enter(qemu_coroutine_create(usb_net_tx_co, sess));
}

static void usb_net_start_co(USBNetUplinkState* s, CoroutineEntry* fn)
{
    qemu_coroutine_enter(qemu_coroutine_create(fn, usb_net_session_new(s)));
}

static void coroutine_fn usb_net_session_co(void* opaque)
{
    g_autoptr(USBNetSession)        sess                           = opaque;
    USBNetUplinkState*              s                              = sess->s;
    uint32_t                        gen                            = sess->generation;
    USBDevice*                      dev                            = usb_uplink_active(&s->usb);
    g_autoptr(USBUplinkDescriptors) desc                           = NULL;
    uint8_t                         mode[4]                        = {0};
    uint8_t                         params[NCM_NTB_PARAMETERS_LEN] = {0};
    uint8_t                         one                            = 0;
    const uint8_t*                  cfg;
    uint32_t                        cfg_len = 0;
    uint32_t                        actual  = 0;
    uint32_t                        in_size;
    uint32_t                        dev_in_max;
    int32_t                         status;
    USBNetUplinkNCM                 ncm;

    if (dev == NULL) { goto out; }
    object_ref(OBJECT(dev));

    qemu_co_sleep_ns(QEMU_CLOCK_REALTIME, USB_NET_SETTLE_MS * SCALE_MS);
    if (!usb_net_session_alive(s, gen)) { goto out_unref; }

    /* Enumerate the way a host would: address it, then read what it offers. */
    status = usb_net_control(s, dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, 1, 0, 0,
                             NULL, NULL);
    if (status != USB_RET_SUCCESS || !usb_net_session_alive(s, gen)) {
        DPRINTF("SET_ADDRESS failed: %d\n", status);
        goto out_unref;
    }

    WITH_QEMU_LOCK_GUARD(&s->usb.ctrl_lock) { desc = usb_uplink_read_descriptors(dev); }
    if (desc == NULL || !usb_net_session_alive(s, gen)) {
        warn_report("usb-net-uplink: could not read the device's descriptors");
        goto out_unref;
    }

    info_report("usb-net-uplink: device %04x:%04x \"%s\", %u configurations", lduw_le_p(desc->device + 8),
                lduw_le_p(desc->device + 10), desc->product ?: "?", desc->num_configs);

    if (lduw_le_p(desc->device + 8) != APPLE_VENDOR_ID) {
        warn_report("usb-net-uplink: not an Apple device; leaving it alone");
        goto out_unref;
    }

    cfg = usb_net_find_config(desc, APPLE_NCM_CONFIGURATION, &cfg_len);
    if (cfg == NULL || !usb_net_parse_ncm(cfg, cfg_len, &ncm)) {
        /* Initial mode: ask for the configuration with CDC-NCM. The device then re-enumerates. */
        status = usb_net_control(s, dev, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, APPLE_REQ_GET_MODE, 0, 0,
                                 sizeof(mode), mode, &actual);
        if (status == USB_RET_SUCCESS && actual >= 4) {
            info_report("usb-net-uplink: device mode %u:%u:%u:%u", mode[0], mode[1], mode[2], mode[3]);
        }

        if (s->mode_switches >= MAX_MODE_SWITCHES) {
            warn_report("usb-net-uplink: no CDC-NCM configuration after %u mode switches; giving up",
                        s->mode_switches);
            goto out_unref;
        }

        s->mode_switches++;
        info_report("usb-net-uplink: switching the device to mode %u (CDC-NCM)", APPLE_MODE_CDC_NCM);
        status = usb_net_control(s, dev, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE, APPLE_REQ_SET_MODE, 0,
                                 APPLE_MODE_CDC_NCM, 1, &one, &actual);
        if (status != USB_RET_SUCCESS) { warn_report("usb-net-uplink: SET_MODE failed (%d)", status); }
        goto out_unref;
    }

    status = usb_net_control(s, dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                             ncm.config_value, 0, 0, NULL, NULL);
    if (status != USB_RET_SUCCESS || !usb_net_session_alive(s, gen)) {
        warn_report("usb-net-uplink: SET_CONFIGURATION %u failed (%d)", ncm.config_value, status);
        goto out_unref;
    }

    status = usb_net_control(s, dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, NCM_REQ_GET_NTB_PARAMETERS, 0,
                             ncm.ctrl_iface, sizeof(params), params, &actual);
    if (status != USB_RET_SUCCESS || actual < NCM_NTB_PARAMETERS_LEN) {
        warn_report("usb-net-uplink: GET_NTB_PARAMETERS failed (%d, %u bytes)", status, actual);
        goto out_unref;
    }

    ncm.ntb_out_max       = ldl_le_p(params + 16);
    ncm.ndp_out_divisor   = lduw_le_p(params + 20);
    ncm.ndp_out_remainder = lduw_le_p(params + 22);
    ncm.ndp_out_alignment = lduw_le_p(params + 24);

    dev_in_max = (uint32_t)ldl_le_p(params + 4);
    in_size    = MIN(dev_in_max, NCM_NTB_IN_SIZE_WANTED);
    if (in_size < 2048) { in_size = dev_in_max; }
    if (in_size != dev_in_max) {
        uint8_t req[4];

        stl_le_p(req, in_size);
        status = usb_net_control(s, dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                 NCM_REQ_SET_NTB_INPUT_SIZE, 0, ncm.ctrl_iface, sizeof(req), req, NULL);
        if (status != USB_RET_SUCCESS) { in_size = dev_in_max; }
    }
    ncm.ntb_in_size = in_size;

    /* NTB16 is the default; say so anyway, as Linux does when the device also offers NTB32. */
    if (lduw_le_p(params + 2) & 2) {
        usb_net_control(s, dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, NCM_REQ_SET_NTB_FORMAT,
                        USB_CDC_NCM_NTB16, ncm.ctrl_iface, 0, NULL, NULL);
    }

    status = usb_net_control(s, dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE,
                             ncm.data_alt, ncm.data_iface, 0, NULL, NULL);
    if (status != USB_RET_SUCCESS || !usb_net_session_alive(s, gen)) {
        warn_report("usb-net-uplink: SET_INTERFACE %u/%u failed (%d)", ncm.data_iface, ncm.data_alt, status);
        goto out_unref;
    }

    s->ncm           = ncm;
    s->mode_switches = 0;
    info_report("usb-net-uplink: CDC-NCM up (config %u, control %u, data %u/%u, in 0x%02x out 0x%02x, NTB in %u out "
                "%u)",
                ncm.config_value, ncm.ctrl_iface, ncm.data_iface, ncm.data_alt, ncm.ep_in, ncm.ep_out,
                ncm.ntb_in_size, ncm.ntb_out_max);

    usb_net_set_link(s, true);
    usb_net_start_co(s, usb_net_rx_co);
    if (ncm.ep_intr != 0) { usb_net_start_co(s, usb_net_intr_co); }

out_unref:
    object_unref(OBJECT(dev));
out:
    s->session_running = false;

    /* The device came back while this session was still winding down: start the next one. */
    if (gen != s->generation && usb_uplink_active(&s->usb) != NULL) { qemu_bh_schedule(s->start_bh); }
}

static void usb_net_start_bh(void* opaque)
{
    USBNetUplinkState* s = opaque;
    USBNetSession*     sess;

    if (s->session_running || usb_uplink_active(&s->usb) == NULL) { return; }

    sess               = usb_net_session_new(s);
    s->session_running = true;
    qemu_coroutine_enter(qemu_coroutine_create(usb_net_session_co, sess));
}

/* Coroutines parked on a cancelled packet are resumed here, outside the detach path. */
static void usb_net_wake_bh(void* opaque)
{
    USBNetUplinkState* s      = opaque;
    USBUplinkPacket*   pkts[] = {&s->rx_pkt, &s->intr_pkt, &s->tx_pkt};

    for (size_t i = 0; i < ARRAY_SIZE(pkts); i++) {
        if (pkts[i]->waiter != NULL) {
            pkts[i]->p.status = USB_RET_NODEV;
            usb_uplink_packet_wake(&pkts[i]->p);
        }
    }

    if (usb_uplink_active(&s->usb) != NULL) { qemu_bh_schedule(s->start_bh); }
}

static void usb_net_cancel(USBUplinkPacket* pkt, bool busy)
{
    if (busy && usb_packet_is_inflight(&pkt->p)) { usb_cancel_packet(&pkt->p); }
}

static void usb_net_uplink_attach(USBPort* port)
{
    USBNetUplinkState* s = port->opaque;

    if (port->dev == NULL || !port->dev->attached) { return; }

    DPRINTF("attach on port %d\n", port->index);
    s->generation++;
    usb_net_sessions_mark_stale(s);
    usb_uplink_invalidate(&s->usb);
    usb_net_set_link(s, false);
    qemu_bh_schedule(s->start_bh);
}

static void usb_net_uplink_detach(USBPort* port)
{
    USBNetUplinkState* s = port->opaque;

    DPRINTF("detach on port %d\n", port->index);
    s->generation++;
    usb_net_sessions_mark_stale(s);
    usb_net_cancel(&s->rx_pkt, s->rx_busy);
    usb_net_cancel(&s->intr_pkt, s->intr_busy);
    usb_net_cancel(&s->tx_pkt, s->tx_busy);
    usb_uplink_invalidate(&s->usb);
    usb_net_set_link(s, false);
    qemu_bh_schedule(s->wake_bh);
}

static void usb_net_uplink_complete(USBPort* port, USBPacket* p) { usb_uplink_packet_wake(p); }

static USBBusOps  usb_net_uplink_bus_ops  = {};
static USBPortOps usb_net_uplink_port_ops = {
    .attach   = usb_net_uplink_attach,
    .detach   = usb_net_uplink_detach,
    .complete = usb_net_uplink_complete,
};

static bool usb_net_uplink_can_receive(NetClientState* nc)
{
    USBNetUplinkState* s = qemu_get_nic_opaque(nc);

    return s->link_up && s->tx_queue_bytes < NCM_TX_QUEUE_MAX_BYTES;
}

static ssize_t usb_net_uplink_receive(NetClientState* nc, const uint8_t* buf, size_t size)
{
    USBNetUplinkState* s = qemu_get_nic_opaque(nc);
    GByteArray*        frame;

    if (!s->link_up) { return size; }
    if (size < ETH_FRAME_MIN || size > ETH_FRAME_MAX) { return size; }
    if (s->tx_queue_bytes >= NCM_TX_QUEUE_MAX_BYTES) { return 0; }

    if (s->tx_frames++ == 0) { info_report("usb-net-uplink: first frame to the guest (%zu bytes)", size); }

    frame = g_byte_array_sized_new(size);
    g_byte_array_append(frame, buf, size);
    g_queue_push_tail(&s->tx_queue, frame);
    s->tx_queue_bytes += size;

    usb_net_kick_tx(s);
    return size;
}

static NetClientInfo usb_net_uplink_nc_info = {
    .type        = NET_CLIENT_DRIVER_NIC,
    .size        = sizeof(NICState),
    .can_receive = usb_net_uplink_can_receive,
    .receive     = usb_net_uplink_receive,
};

static void usb_net_uplink_realize(DeviceState* dev, Error** errp)
{
    USBNetUplinkState* s = USB_NET_UPLINK(dev);

    if (s->conf.peers.ncs[0] == NULL) {
        error_setg(errp, "usb-uplink-type=net needs a network backend: pass usb-uplink-addr=<netdev id> and a "
                         "matching -netdev");
        return;
    }

    usb_uplink_init(&s->usb, dev, &usb_net_uplink_bus_ops, &usb_net_uplink_port_ops, s);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&usb_net_uplink_nc_info, &s->conf, object_get_typename(OBJECT(dev)), dev->id, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    g_queue_init(&s->tx_queue);
    s->start_bh = qemu_bh_new(usb_net_start_bh, s);
    s->wake_bh  = qemu_bh_new(usb_net_wake_bh, s);
}

static void usb_net_uplink_unrealize(DeviceState* dev)
{
    USBNetUplinkState* s = USB_NET_UPLINK(dev);

    s->generation++;
    usb_net_sessions_mark_stale(s);
    g_clear_pointer(&s->start_bh, qemu_bh_delete);
    g_clear_pointer(&s->wake_bh, qemu_bh_delete);
    g_queue_clear_full(&s->tx_queue, usb_net_uplink_free_frame);
    if (s->nic != NULL) { qemu_del_nic(s->nic); }
}

static const Property usb_net_uplink_props[] = {
    DEFINE_NIC_PROPERTIES(USBNetUplinkState, conf),
};

static void usb_net_uplink_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass* dc = DEVICE_CLASS(klass);

    dc->realize   = usb_net_uplink_realize;
    dc->unrealize = usb_net_uplink_unrealize;
    dc->desc      = "USB host for the device's CDC-NCM function, bridged to a netdev";
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    device_class_set_props(dc, usb_net_uplink_props);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(USBNetUplinkState, usb_net_uplink, USB_NET_UPLINK, SYS_BUS_DEVICE)
