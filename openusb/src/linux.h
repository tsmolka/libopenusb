/*
 * Linux USB support
 *
 *	Copyright 2007 Michael Lewis <milewis1@gmail.com>
 *	Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 *	This library is covered by the LGPL, read LICENSE for details.
 */

#ifndef __LINUX_H__
#define __LINUX_H__

#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "libusb.h"
#include "usbi.h"

/*
 * These structures MUST match up to the structures in the kernel. They are
 * used solely to communicate with the kernel.
 */
struct usbk_ctrltransfer {
  /* keep in sync with usb.h:usb_proc_ctrltransfer */
  u_int8_t  bRequestType;
  u_int8_t  bRequest;
  u_int16_t wValue;
  u_int16_t wIndex;
  u_int16_t wLength;

  u_int32_t timeout;	/* in milliseconds */

  /* pointer to data */
  void *data;
};

struct usbk_bulktransfer {
  /* keep in sync with usb.h:usb_proc_bulktransfer */
  unsigned int ep;
  unsigned int len;
  unsigned int timeout;	/* in milliseconds */

  /* pointer to data */
  void *data;
};

struct usbk_setinterface {
  unsigned int interface;
  unsigned int altsetting;
};

struct usbk_connectinfo {
  unsigned int devnum;
  unsigned char slow;
};

#define USBK_MAXDRIVERNAME 255

struct usbk_getdriver {
  unsigned int interface;
  char driver[USBK_MAXDRIVERNAME + 1];
};

#define USBK_URB_DISABLE_SPD	0x01
#define USBK_URB_ISO_ASAP	0x02

#define USBK_URB_TYPE_ISO	0
#define USBK_URB_TYPE_INTERRUPT	1
#define USBK_URB_TYPE_CONTROL	2
#define USBK_URB_TYPE_BULK	3

struct usbk_iso_packet_desc {
  unsigned int length;
  unsigned int actual_length;
  unsigned int status;
};

struct usbk_urb {
  unsigned char type;
  unsigned char endpoint;
  int status;
  unsigned int flags;
  void *buffer;
  int buffer_length;
  int actual_length;
  int start_frame;
  int number_of_packets;
  int error_count;
  unsigned int signr; /* signal to be sent on error, 0 if none should be sent */
  void *usercontext;
  struct usbk_iso_packet_desc iso_frame_desc[0];
};

struct usbk_ioctl {
  int ifno;		/* interface 0..N ; negative numbers reserved */
  int ioctl_code;	/* MUST encode size + direction of data so the
			 * macros in <asm/ioctl.h> give correct values */
  void *data;		/* param buffer (in, or out) */
};

/*
 * NOTE: This differs from the kernel in the sign of the types, but this is
 * how it should be.
 */
struct usbk_hub_portinfo {
  unsigned char numports;
  unsigned char port[127];	/* port to device num mapping */
};


#define USB_MAX_DEVICES_PER_BUS	128
#define ISOC_URB_MAX_NUM				10
#define RESUBMIT								1
#define NO_RESUBMIT							0
#define WAKEUP									0x00	/* wakeup the io thread */
#define WAKEUPANDEXIT						0xFF	/* wakeup and exit the io thread */

/*
 * IOCTL Definitions
 */
#define IOCTL_USB_CONTROL	      _IOWR('U', 0, struct usbk_ctrltransfer)
#define IOCTL_USB_BULK		      _IOWR('U', 2, struct usbk_bulktransfer)
#define IOCTL_USB_RESETEP	      _IOR('U', 3, unsigned int)
#define IOCTL_USB_SETINTF	      _IOR('U', 4, struct usbk_setinterface)
#define IOCTL_USB_SETCONFIG     _IOR('U', 5, unsigned int)
#define IOCTL_USB_GETDRIVER     _IOW('U', 8, struct usbk_getdriver)
#define IOCTL_USB_SUBMITURB     _IOR('U', 10, struct usbk_urb)
#define IOCTL_USB_DISCARDURB    _IO('U', 11)
#define IOCTL_USB_REAPURB       _IOW('U', 12, void *)
#define IOCTL_USB_REAPURBNDELAY _IOW('U', 13, void *)
#define IOCTL_USB_CLAIMINTF     _IOR('U', 15, unsigned int)
#define IOCTL_USB_RELEASEINTF   _IOR('U', 16, unsigned int)
#define IOCTL_USB_CONNECTINFO   _IOW('U', 17, struct usbk_connectinfo)
#define IOCTL_USB_IOCTL         _IOWR('U', 18, struct usbk_ioctl)
#define IOCTL_USB_HUB_PORTINFO  _IOR('U', 19, struct usbk_hub_portinfo)
#define IOCTL_USB_RESET         _IO('U', 20)
#define IOCTL_USB_CLEAR_HALT    _IOR('U', 21, unsigned int)
#define IOCTL_USB_DISCONNECT    _IO('U', 22)
#define IOCTL_USB_CONNECT       _IO('U', 23)
/*
 * IOCTL_USB_HUB_PORTINFO, IOCTL_USB_DISCONNECT and IOCTL_USB_CONNECT
 * all work via IOCTL_USB_IOCTL
 */



/* Thread Functions */
void *poll_io(void *usbihdl);
void *poll_events(void *unused);



/* Helper Functions */
struct usbi_io* isoc_io_clone(struct usbi_io *io);
int32_t urb_submit(struct usbi_dev_handle *hdev, struct usbi_io *io);
int32_t io_complete(struct usbi_dev_handle *hdev);
int32_t io_timeout(struct usbi_dev_handle *hdev, struct timeval *tvc);
int32_t create_new_device(struct usbi_device **dev, struct usbi_bus *ibus,
                          uint16_t devnum, uint32_t max_children);
int32_t check_usb_path(const char *dirname);
int32_t translate_errno(int errnum);
int32_t wakeup_io_thread(struct usbi_dev_handle *hdev, uint8_t command);
int32_t linux_get_driver(struct usbi_dev_handle *hdev, uint8_t interface,
												 char *name, size_t namelen);
int32_t linux_attach_kernel_driver(struct usbi_dev_handle *hdev,
																	 uint8_t interface);
int32_t linux_detach_kernel_driver(struct usbi_dev_handle *hdev,
																	 uint8_t interface);



/* Linux specific members for various internal structures */
struct usbi_bus_private
{
	char                filename[PATH_MAX + 1];
	struct usbi_device  *dev_by_num[USB_MAX_DEVICES_PER_BUS];
};


struct usbi_dev_private
{
	char    filename[PATH_MAX + 1];   /* full path to usbfs file */
	time_t  mtime;                    /* modify time to detect dev changes */
	int     found;                    /* flag to denote if we saw this dev during rescan */
};


struct usbi_dev_hdl_private
{
	int             fd;            /* file descriptor for usbdevfs entry */
	int             event_pipe[2]; /* let's us know when things are happening */
	int16_t					reattachdrv;	 /* do we need to reattach the kernel driver */
	pthread_t       io_thread;     /* thread for processing io requests */
};


struct usbi_io_private
{
	struct usbk_urb urb;        /* URB for IOCTLs */
	void *tempbuf;							/* temporary data storage */
};


#define LIBUSB_HAS_GET_DRIVER_NP 1
#define LIBUSB_HAS_DETACH_KERNEL_DRIVER_NP 1

#endif

