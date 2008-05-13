/*
 * Data structures and function prototypes of Solaris backend
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */
#ifndef	__SUNOS_H__
#define	__SUNOS_H__

#include <libdevinfo.h>
#include <pthread.h>
#include "openusb.h"
#include "usbi.h"

#define	APID_NAMELEN		32 /* max len in cfgadm display */

#define	USBI_MAXINTERFACES	32

/* Solaris device private information */
struct usb_dev_info {
	int ref_count;
	int ep0_fd;
	int ep0_fd_stat;

	/* remember which dev_handle has claimed this interface */
	struct usbi_dev_handle *claimed_interfaces[USBI_MAXINTERFACES];
};


struct usbk_isoc_pkt_descr {
	ushort_t isoc_pkt_length;
	ushort_t isoc_pkt_actual_length;
	uint_t isoc_pkt_status;
};

struct usbk_isoc_pkt_header {
	int	isoc_pkts_count; /* pkt count of an isoc req */
	struct usbk_isoc_pkt_descr isoc_pkts_dsc[1]; /* length of all pkts */
};

struct usb_isoc_io {
	void *buf;
	size_t buflen;
};

#define SUNOS_BUS_EHCI 0x01
#define SUNOS_BUS_OHCI 0x02
#define SUNOS_BUS_UHCI 0x03


struct usbi_bus_private {
	di_node_t node;
	unsigned char model; /* ehci, uhci, or ohci */
};

struct usbi_dev_private {
	char *devlink; /* device link */
	char *ugenpath; /* path to ugen node directory */
	char *ap_ancestry; /* ancestry string for cfgadm ap_id for hub */
	time_t mtime; /* modify time to detect dev changes */
	int found; /* flag to denote if we saw this dev during rescan */
	struct usb_dev_info info; /* stored file descrs of eps */

	char *udi; /* HAL udi of this device */
};

struct ep {
	int datafd; /* data file descriptor */
	int statfd; /* status file descriptor */
};

#define	USBI_MAXENDPOINTS	32


#define ISOC_IN_INITED	0x01

struct usbi_dev_hdl_private{
	int config_index; /*index of the current config in desc->configs[] */
	struct ep eps[USBI_MAXENDPOINTS]; /* opened endpoints */

	/* Now, used by isoc IN endpoints to flag if request has been set */
	uchar_t epflags[USBI_MAXENDPOINTS]; 

	int ep_interface[USBI_MAXENDPOINTS];

	int	statfd; /* devstat fd, to be polled for device hotplug events */
	pthread_t pollthr;

	pthread_t timeout_thr;
};

#define	USBI_IO_HANDLE_PRIVATE \
	struct usb_isoc_io isoc_io;

struct devlink_cbarg {
	struct usbi_device *idev;
	di_minor_t minor;
};

struct hubd_ioctl_data {
	uint_t		cmd;			/* cfgadm sub command */
	uint_t		port;			/* port of (root)hub */
	uint_t		get_size;		/* get size/data flag */
	caddr_t		buf;			/* data buffer */
	uint_t		bufsiz;			/* data buffer size */
	uint_t		misc_arg;		/* reserved */
};

/* I/O direction */
#define	READ	0
#define	WRITE	1

/* cfgadm devctl subcommand */
#define	USB_DESCR_TYPE_DEV			0x01
#define	USB_DESCR_TYPE_CFG			0x02

#endif /* __SUNOS_H__ */
