/*
 * USB uplink that acts as the host for the emulated device's CDC-NCM function
 * and bridges it to a QEMU network backend.
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
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "hw/usb/usb-uplink.h"
#include "net/net.h"
#include "qemu/coroutine.h"
#include "qom/object.h"

#define TYPE_USB_NET_UPLINK "usb-net-uplink"
OBJECT_DECLARE_SIMPLE_TYPE(USBNetUplinkState, USB_NET_UPLINK)

typedef struct USBNetSession USBNetSession;

/* What the session has learnt about the device's NCM function. */
typedef struct USBNetUplinkNCM
{
    uint8_t  config_value;
    uint8_t  ctrl_iface;
    uint8_t  data_iface;
    uint8_t  data_alt;
    uint8_t  ep_in;
    uint8_t  ep_out;
    uint8_t  ep_intr;
    uint8_t  mac_string;
    uint16_t max_segment;

    /* From GET_NTB_PARAMETERS. */
    uint32_t ntb_in_size;
    uint32_t ntb_out_max;
    uint16_t ndp_out_divisor;
    uint16_t ndp_out_remainder;
    uint16_t ndp_out_alignment;
} USBNetUplinkNCM;

struct USBNetUplinkState
{
    SysBusDevice parent_obj;

    USBUplinkDevice usb;
    NICConf         conf;
    NICState*       nic;

    /* Bumped whenever the device comes or goes; a session stops when it moves. */
    uint32_t generation;
    QEMUBH*  start_bh;
    QEMUBH*  wake_bh;
    bool     session_running;
    QLIST_HEAD(, USBNetSession) sessions;
    uint32_t mode_switches;

    USBNetUplinkNCM ncm;
    bool            link_up;
    uint16_t        tx_seq;
    uint64_t        rx_frames;
    uint64_t        tx_frames;

    /* In-flight packets, so a detach can cancel them and wake their coroutines. */
    USBUplinkPacket rx_pkt;
    USBUplinkPacket intr_pkt;
    USBUplinkPacket tx_pkt;
    bool            rx_busy;
    bool            intr_busy;
    bool            tx_busy;

    GQueue   tx_queue;
    uint32_t tx_queue_bytes;
    bool     tx_running;
};
