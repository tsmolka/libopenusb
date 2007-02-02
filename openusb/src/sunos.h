#ifndef	__SUNOS_H__
#define	__SUNOS_H__

#include <libdevinfo.h>

#define	APID_NAMELEN		32 /* max len in cfgadm display */

#define	USBI_MAXINTERFACES	32
struct usb_dev_info {
	int ref_count;
	int ep0_fd;
	int ep0_fd_stat;
	struct usbi_dev_handle *claimed_interfaces[USBI_MAXINTERFACES];
};

struct usbk_isoc_pkt_header {
	ushort_t isoc_pkts_count; /* pkt count of an isoc req */
	uint_t isoc_pkts_length; /* length of all pkts */
};

struct usbk_isoc_pkt_descr {
	ushort_t isoc_pkt_length;
	ushort_t isoc_pkt_actual_length;
	uint_t isoc_pkt_status;
};

struct usb_isoc_io {
	void *buf;
	size_t buflen;
};

#define USBI_BUS_PRIVATE \
	char filename[PATH_MAX + 1]; \
	struct usbi_device *dev_by_num[USB_MAX_DEVICES_PER_BUS]; \
	di_node_t node;

#define USBI_DEVICE_PRIVATE \
	char devpath[PATH_MAX + 1]; /* physical path of usb device */ \
	char *devlink; /* device link */ \
	char *ugenpath; /* path to ugen node directory */ \
	char *ap_ancestry; /* ancestry string for cfgadm ap_id for hub */ \
	int port; /* port number to its parent hub */ \
	time_t mtime; /* modify time to detect dev changes */		\
	int found; /* flag to denote if we saw this dev during rescan */ \
	struct usb_dev_info info; /* stored file descrs of eps */

#define	USBI_MAXENDPOINTS	32
#define	USBI_DEV_HANDLE_PRIVATE \
	int config_value; \
	int config_index; \
	int ep_fd[USBI_MAXENDPOINTS]; \
	int ep_status_fd[USBI_MAXENDPOINTS]; \
	int ep_interface[USBI_MAXENDPOINTS];

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

struct usb_dev_descr {
	uint8_t			bLength;	/* descriptor size */
	uint8_t			bDescriptorType;/* set to DEVICE */
	struct usb_device_desc	desc;		/* remained */
};

/* I/O direction */
#define	READ	0
#define	WRITE	1

/* cfgadm devctl subcommand */
#define	USB_DESCR_TYPE_DEV			0x01
#define	USB_DESCR_TYPE_CFG			0x02


#endif /* __SUNOS_H__ */
