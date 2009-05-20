/*
 * Mac OS X USB Support
 *
 *  Copyright 2002-2008 Nathan Hjelm <hjelmn@users.sourceforge.net>
 *	Copyright 2008 Michael Lewis <milewis1@gmail.com>
 *
 *	This library is covered by the LGPL, read LICENSE for details.
 */

#ifndef __MACOSX_H__
#define __MACOSX_H__

#include <IOKit/IOCFBundle.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

#include "openusb.h"
#include "usbi.h"


/* IOUSBInterfaceInferface */
#if defined (kIOUSBInterfaceInterfaceID220)

// #warning "libusb being compiled for 10.4 or later"
#define usb_interface_t IOUSBInterfaceInterface220
#define InterfaceInterfaceID kIOUSBInterfaceInterfaceID220
#define InterfaceVersion 220

#elif defined (kIOUSBInterfaceInterfaceID197)

// #warning "libusb being compiled for 10.3 or later"
#define usb_interface_t IOUSBInterfaceInterface197
#define InterfaceInterfaceID kIOUSBInterfaceInterfaceID197
#define InterfaceVersion 197

#elif defined (kIOUSBInterfaceInterfaceID190)

// #warning "libusb being compiled for 10.2 or later"
#define usb_interface_t IOUSBInterfaceInterface190
#define InterfaceInterfaceID kIOUSBInterfaceInterfaceID190
#define InterfaceVersion 190

#elif defined (kIOUSBInterfaceInterfaceID182)

// #warning "libusb being compiled for 10.1 or later"
#define usb_interface_t IOUSBInterfaceInterface182
#define InterfaceInterfaceID kIOUSBInterfaceInterfaceID182
#define InterfaceVersion 182

#else

/* No timeout functions available! Time to upgrade your os. */
#warning "libopenusb being compiled without support for timeouts! 10.0 and up"
#define usb_interface_t IOUSBInterfaceInterface
#define InterfaceInterfaceID kIOUSBInterfaceInterfaceID
#define LIBOPENUSB_NO_TIMEOUT_INTERFACE
#define InterfaceVersion 180

#endif

/* IOUSBDeviceInterface */
#if defined (kIOUSBDeviceInterfaceID197)

#define usb_device_t    IOUSBDeviceInterface197
#define DeviceInterfaceID kIOUSBDeviceInterfaceID197
#define DeviceVersion 197

#elif defined (kIOUSBDeviceInterfaceID187)

#define usb_device_t    IOUSBDeviceInterface187
#define DeviceInterfaceID kIOUSBDeviceInterfaceID187
#define DeviceVersion 187

#elif defined (kIOUSBDeviceInterfaceID182)

#define usb_device_t    IOUSBDeviceInterface182
#define DeviceInterfaceID kIOUSBDeviceInterfaceID182
#define DeviceVersion 182

#else

#define usb_device_t    IOUSBDeviceInterface
#define DeviceInterfaceID kIOUSBDeviceInterfaceID
#define LIBUSB_NO_TIMEOUT_DEVICE
#define LIBUSB_NO_SEIZE_DEVICE
#define DeviceVersion 180

#endif

typedef IOCFPlugInInterface *io_cf_plugin_ref_t;
typedef IONotificationPortRef io_notification_port_t;

/* OS X specific members for various internal structures */
//struct usbi_bus_private
//{
//	struct usbi_device  *dev_by_num[USB_MAX_DEVICES_PER_BUS];
//};


struct usbi_dev_private {
  int32_t	location;
};


struct darwin_interface {
  usb_interface_t  **interface;
  uint8_t            endpoint_addrs[USBI_MAXENDPOINTS];
  uint8_t            num_endpoints;
  CFRunLoopSourceRef cfSource;
};

struct usbi_dev_hdl_private
{
  usb_device_t            **device;                             /* our device interface */
  struct darwin_interface   interfaces[USBI_MAXINTERFACES];
  CFRunLoopSourceRef        cfSource;                           /* device cf source */
  int                       open;                               /* device is open */
};

struct usbi_bus_private
{
	int32_t location;
};

struct usbi_io_private {
#if !defined (OPENUSB_NO_TIMEOUT_DEVICE)
	IOUSBDevRequestTO req;
#else
	IOUSBDevRequest req;
#endif
	uint8_t			is_read;
	uint8_t*		isoc_buffer;
	IOUSBIsocFrame* isoc_framelist;
};

int32_t darwin_claim_interface (struct usbi_dev_handle *hdev, uint8_t ifc, openusb_init_flag_t flags);
int32_t darwin_release_interface (struct usbi_dev_handle *hdev, uint8_t ifc);
int32_t darwin_submit_ctrl (struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern);
int32_t darwin_submit_bulk_intr (struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern);
int32_t darwin_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern);
int32_t darwin_clear_halt (struct usbi_dev_handle *hdev, uint8_t ept);

#endif

