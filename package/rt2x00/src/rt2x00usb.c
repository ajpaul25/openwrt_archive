/*
	Copyright (C) 2004 - 2007 rt2x00 SourceForge Project
	<http://rt2x00.serialmonkey.com>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the
	Free Software Foundation, Inc.,
	59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
	Module: rt2x00usb
	Abstract: rt2x00 generic usb device routines.
	Supported chipsets: rt2570, rt2571W & rt2671.
 */

/*
 * Set enviroment defines for rt2x00.h
 */
#define DRV_NAME "rt2x00usb"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/usb.h>

#include "rt2x00.h"
#include "rt2x00usb.h"

/*
 * Interfacing with the HW.
 */
int rt2x00usb_vendor_request(const struct rt2x00_dev *rt2x00dev,
	const u8 request, const u8 type, const u16 offset,
	u32 value, void *buffer, const u16 buffer_length, const u16 timeout)
{
	struct usb_device *usb_dev = interface_to_usbdev(
		rt2x00dev_usb(rt2x00dev));
	int status;
	unsigned int i;

	for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
		status = usb_control_msg(
			usb_dev,
			(type == USB_VENDOR_REQUEST_IN) ?
				usb_rcvctrlpipe(usb_dev, 0) :
				usb_sndctrlpipe(usb_dev, 0),
			request, type, value, offset, buffer, buffer_length,
			timeout);
		if (status >= 0)
			return 0;
	}

	ERROR(rt2x00dev, "vendor request error. Request 0x%02x failed "
		"for offset 0x%04x with error %d.\n", request, offset, status);

	return status;
}
EXPORT_SYMBOL_GPL(rt2x00usb_vendor_request);

/*
 * Radio handlers
 */
void rt2x00usb_enable_radio(struct rt2x00_dev *rt2x00dev)
{
	unsigned int i;

	/*
	 * Start the RX ring.
	 */
	for (i = 0; i < rt2x00dev->rx->stats.limit; i++) {
		__set_bit(ENTRY_OWNER_NIC, &rt2x00dev->rx->entry[i].flags);
		usb_submit_urb(rt2x00dev->rx->entry[i].priv, GFP_ATOMIC);
	}
}
EXPORT_SYMBOL_GPL(rt2x00usb_enable_radio);

void rt2x00usb_disable_radio(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;
	unsigned int i;

	rt2x00usb_vendor_request(rt2x00dev, USB_RX_CONTROL,
		USB_VENDOR_REQUEST_OUT, 0x00, 0x00, NULL, 0, REGISTER_TIMEOUT);

	/*
	 * Cancel all rings.
	 */
	ring_for_each(rt2x00dev, ring) {
		for (i = 0; i < ring->stats.limit; i++)
			usb_kill_urb(ring->entry[i].priv);
	}
}
EXPORT_SYMBOL_GPL(rt2x00usb_disable_radio);

/*
 * Beacon handlers.
 */
int rt2x00usb_beacon_update(struct ieee80211_hw *hw, struct sk_buff *skb,
	struct ieee80211_tx_control *control)
{
	struct rt2x00_dev *rt2x00dev = hw->priv;
	struct usb_device *usb_dev =
		interface_to_usbdev(rt2x00dev_usb(rt2x00dev));
	struct data_ring *ring =
		rt2x00_get_ring(rt2x00dev, IEEE80211_TX_QUEUE_BEACON);
	struct data_entry *beacon;
	struct data_entry *guardian;
	int length;

	/*
	 * Just in case the ieee80211 doesn't set this,
	 * but we need this queue set for the descriptor
	 * initialization.
	 */
	control->queue = IEEE80211_TX_QUEUE_BEACON;

	/*
	 * Obtain 2 entries, one for the guardian byte,
	 * the second for the actual beacon.
	 */
	guardian = rt2x00_get_data_entry(ring);
	rt2x00_ring_index_inc(ring);
	beacon = rt2x00_get_data_entry(ring);

	/*
	 * First we create the beacon.
	 */
	skb_push(skb, ring->desc_size);
	rt2x00lib_write_tx_desc(rt2x00dev, beacon,
		(struct data_desc*)skb->data,
		(struct ieee80211_hdr*)(skb->data + ring->desc_size),
		skb->len - ring->desc_size,
		control);

	/*
	 * Length passed to usb_fill_urb cannot be an odd number,
	 * so add 1 byte to make it even.
	 */
	length = skb->len;
	if (length % 2)
		length++;

	usb_fill_bulk_urb(
		beacon->priv,
		usb_dev,
		usb_sndbulkpipe(usb_dev, 1),
		skb->data,
		length,
		rt2x00usb_beacondone,
		beacon);

	beacon->skb = skb;

	/*
	 * Second we need to create the guardian byte.
	 * We only need a single byte, so lets recycle
	 * the 'flags' field we are not using for beacons.
	 */
	guardian->flags = 0;
	usb_fill_bulk_urb(
		guardian->priv,
		usb_dev,
		usb_sndbulkpipe(usb_dev, 1),
		&guardian->flags,
		1,
		rt2x00usb_beacondone,
		guardian);

	/*
	 * Send out the guardian byte.
	 */
	usb_submit_urb(guardian->priv, GFP_ATOMIC);

	/*
	 * Enable beacon generation.
	 */
	rt2x00dev->ops->lib->kick_tx_queue(rt2x00dev, control->queue);

	return 0;
}
EXPORT_SYMBOL_GPL(rt2x00usb_beacon_update);

void rt2x00usb_beacondone(struct urb *urb)
{
	struct data_entry *entry = (struct data_entry*)urb->context;
	struct data_ring *ring = entry->ring;

	if (!test_bit(DEVICE_ENABLED_RADIO, &ring->rt2x00dev->flags))
		return;

	/*
	 * Check if this was the guardian beacon,
	 * if that was the case we need to send the real beacon now.
	 * Otherwise we should free the sk_buffer, the device
	 * should be doing the rest of the work now.
	 */
	if (ring->index == 1) {
		rt2x00_ring_index_done_inc(ring);
		entry = rt2x00_get_data_entry(ring);
		usb_submit_urb(entry->priv, GFP_ATOMIC);
		rt2x00_ring_index_inc(ring);
	} else if (ring->index_done == 1) {
		entry = rt2x00_get_data_entry_done(ring);
		if (entry->skb) {
			dev_kfree_skb(entry->skb);
			entry->skb = NULL;
		}
		rt2x00_ring_index_done_inc(ring);
	}
}
EXPORT_SYMBOL_GPL(rt2x00usb_beacondone);

/*
 * TX data handlers.
 */
static void rt2x00usb_interrupt_txdone(struct urb *urb)
{
	struct data_entry *entry = (struct data_entry*)urb->context;
	struct data_ring *ring = entry->ring;
	struct rt2x00_dev *rt2x00dev = ring->rt2x00dev;
	struct data_desc *txd = (struct data_desc *)entry->skb->data;
	u32 word;
	int tx_status;

	if (!test_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags) ||
	    !__test_and_clear_bit(ENTRY_OWNER_NIC, &entry->flags))
		return;

	rt2x00_desc_read(txd, 0, &word);

	/*
	 * Remove the descriptor data from the buffer.
	 */
	skb_pull(entry->skb, ring->desc_size);

	/*
	 * Obtain the status about this packet.
	 */
	tx_status = !urb->status ? TX_SUCCESS : TX_FAIL_RETRY;

	rt2x00lib_txdone(entry, tx_status, 0);

	/*
	 * Make this entry available for reuse.
	 */
	entry->flags = 0;
	rt2x00_ring_index_done_inc(entry->ring);

	/*
	 * If the data ring was full before the txdone handler
	 * we must make sure the packet queue in the mac80211 stack
	 * is reenabled when the txdone handler has finished.
	 */
	if (!rt2x00_ring_full(ring))
		ieee80211_wake_queue(rt2x00dev->hw,
			entry->tx_status.control.queue);
}

int rt2x00usb_write_tx_data(struct rt2x00_dev *rt2x00dev,
	struct data_ring *ring, struct sk_buff *skb,
	struct ieee80211_tx_control *control)
{
	struct usb_device *usb_dev =
		interface_to_usbdev(rt2x00dev_usb(rt2x00dev));
	struct ieee80211_hdr *ieee80211hdr = (struct ieee80211_hdr*)skb->data;
	struct data_entry *entry = rt2x00_get_data_entry(ring);
	struct data_desc *txd;
	u32 length = skb->len;

	if (rt2x00_ring_full(ring)) {
		ieee80211_stop_queue(rt2x00dev->hw, control->queue);
		return -EINVAL;
	}

	if (test_bit(ENTRY_OWNER_NIC, &entry->flags)) {
		ERROR(rt2x00dev,
			"Arrived at non-free entry in the non-full queue %d.\n"
			"Please file bug report to %s.\n",
			control->queue, DRV_PROJECT);
		ieee80211_stop_queue( rt2x00dev->hw, control->queue);
		return -EINVAL;
	}

	skb_push(skb, rt2x00dev->hw->extra_tx_headroom);
	txd = (struct data_desc*)skb->data;
	rt2x00lib_write_tx_desc(rt2x00dev, entry, txd, ieee80211hdr,
		length, control);
	memcpy(&entry->tx_status.control, control, sizeof(*control));
	entry->skb = skb;

	/*
	 * Length passed to usb_fill_urb cannot be an odd number,
	 * so add 1 byte to make it even.
	 */
	length += rt2x00dev->hw->extra_tx_headroom;
	if (length % 2)
		length++;

	__set_bit(ENTRY_OWNER_NIC, &entry->flags);
	usb_fill_bulk_urb(
		entry->priv,
		usb_dev,
		usb_sndbulkpipe(usb_dev, 1),
		skb->data,
		length,
		rt2x00usb_interrupt_txdone,
		entry);
	usb_submit_urb(entry->priv, GFP_ATOMIC);

	rt2x00_ring_index_inc(ring);

	if (rt2x00_ring_full(ring))
		ieee80211_stop_queue(rt2x00dev->hw, control->queue);

	return 0;
}
EXPORT_SYMBOL_GPL(rt2x00usb_write_tx_data);

/*
 * Device initialization handlers.
 */
static int rt2x00usb_alloc_ring(struct rt2x00_dev *rt2x00dev,
	struct data_ring *ring)
{
	unsigned int i;

	/*
	 * Allocate the URB's
	 */
	for (i = 0; i < ring->stats.limit; i++) {
		ring->entry[i].priv = usb_alloc_urb(0, GFP_KERNEL);
		if (!ring->entry[i].priv)
			return -ENOMEM;
	}

	return 0;
}

int rt2x00usb_initialize(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;
	struct sk_buff *skb;
	unsigned int entry_size;
	unsigned int i;
	int status;

	/*
	 * Allocate DMA
	 */
	ring_for_each(rt2x00dev, ring) {
		status = rt2x00usb_alloc_ring(rt2x00dev, ring);
		if (status)
			goto exit;
	}

	/*
	 * For the RX ring, skb's should be allocated.
	 */
	entry_size = ring->data_size + ring->desc_size;
	for (i = 0; i < rt2x00dev->rx->stats.limit; i++) {
		skb = dev_alloc_skb(NET_IP_ALIGN + entry_size);
		if (!skb)
			goto exit;

		skb_reserve(skb, NET_IP_ALIGN);
		skb_put(skb, entry_size);

		rt2x00dev->rx->entry[i].skb = skb;
	}

	return 0;

exit:
	rt2x00usb_uninitialize(rt2x00dev);

	return status;
}
EXPORT_SYMBOL_GPL(rt2x00usb_initialize);

void rt2x00usb_uninitialize(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;
	unsigned int i;

	ring_for_each(rt2x00dev, ring) {
		if (!ring->entry)
			continue;

		for (i = 0; i < ring->stats.limit; i++) {
			usb_kill_urb(ring->entry[i].priv);
			usb_free_urb(ring->entry[i].priv);
			if (ring->entry[i].skb)
				kfree_skb(ring->entry[i].skb);
		}
	}
}
EXPORT_SYMBOL_GPL(rt2x00usb_uninitialize);

/*
 * USB driver handlers.
 */
int rt2x00usb_probe(struct usb_interface *usb_intf,
	const struct usb_device_id *id)
{
	struct usb_device *usb_dev = interface_to_usbdev(usb_intf);
	struct rt2x00_ops *ops = (struct rt2x00_ops*)id->driver_info;
	struct ieee80211_hw *hw;
	struct rt2x00_dev *rt2x00dev;
	int retval;

	usb_dev = usb_get_dev(usb_dev);

	hw = ieee80211_alloc_hw(sizeof(struct rt2x00_dev), ops->hw);
	if (!hw) {
		ERROR_PROBE("Failed to allocate hardware.\n");
		retval = -ENOMEM;
		goto exit_put_device;
	}

	usb_set_intfdata(usb_intf, hw);

	rt2x00dev = hw->priv;
	rt2x00dev->dev = usb_intf;
	rt2x00dev->device = &usb_intf->dev;
	rt2x00dev->ops = ops;
	rt2x00dev->hw = hw;

	retval = rt2x00lib_probe_dev(rt2x00dev);
	if (retval)
		goto exit_free_device;

	return 0;

exit_free_device:
	ieee80211_free_hw(hw);

exit_put_device:
	usb_put_dev(usb_dev);

	usb_set_intfdata(usb_intf, NULL);

	return retval;
}
EXPORT_SYMBOL_GPL(rt2x00usb_probe);

void rt2x00usb_disconnect(struct usb_interface *usb_intf)
{
	struct ieee80211_hw *hw = usb_get_intfdata(usb_intf);
	struct rt2x00_dev *rt2x00dev = hw->priv;

	/*
	 * Free all allocated data.
	 */
	rt2x00lib_remove_dev(rt2x00dev);
	ieee80211_free_hw(hw);

	/*
	 * Free the USB device data.
	 */
	usb_set_intfdata(usb_intf, NULL);
	usb_put_dev(interface_to_usbdev(usb_intf));
}
EXPORT_SYMBOL_GPL(rt2x00usb_disconnect);

#ifdef CONFIG_PM
int rt2x00usb_suspend(struct usb_interface *usb_intf, pm_message_t state)
{
	struct ieee80211_hw *hw = usb_get_intfdata(usb_intf);
	struct rt2x00_dev *rt2x00dev = hw->priv;
	int retval;

	retval = rt2x00lib_suspend(rt2x00dev, state);
	if (retval)
		return retval;

	/*
	 * Decrease usbdev refcount.
	 */
	usb_put_dev(interface_to_usbdev(usb_intf));

	return 0;
}
EXPORT_SYMBOL_GPL(rt2x00usb_suspend);

int rt2x00usb_resume(struct usb_interface *usb_intf)
{
	struct ieee80211_hw *hw = usb_get_intfdata(usb_intf);
	struct rt2x00_dev *rt2x00dev = hw->priv;

	usb_get_dev(interface_to_usbdev(usb_intf));

	return rt2x00lib_resume(rt2x00dev);
}
EXPORT_SYMBOL_GPL(rt2x00usb_resume);
#endif /* CONFIG_PM */

/*
 * rt2x00pci module information.
 */
MODULE_AUTHOR(DRV_PROJECT);
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("rt2x00 library");
MODULE_LICENSE("GPL");
