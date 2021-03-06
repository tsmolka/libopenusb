/*
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 *
 */
#include <string.h>
#include <usb.h> /* openusb 0.1.x header file. Its path is OS dependent */
#include <pthread.h>
#include <errno.h>

#include "usbi.h"

#ifdef HAVE_CONFIG_H
#include "config.h" /* for platform specific stuff */
#endif

/* One bit of weirdness that I've been dealing with for years now.  Linux
 * finally got it right, now mac is lagging behind...
 */
#if __APPLE__
	#define USBCONSTISSUE
#else
	#define USBCONSTISSUE const
#endif

/*
 * Libusb1.0 to 0.1.x conversion layer
 * This layer provides backward compatibility for existing openusb
 * applications.
 *
 * This wrapper layer exports openusb 0.1.x interfaces. Basically, the
 * implementation behind these interfaces are based on openusb 1.0 APIs.
 *
 * Return values of the wrapper layer interfaces may be confused.
 */
openusb_handle_t wr_handle = 0;

struct usb_bus *usb_busses = NULL;

static int wr_setup_dev_config(struct usb_device *dev, openusb_devid_t devid,
	openusb_dev_handle_t);

/* process openusb0.1.x error strings */
typedef enum {
	USB_ERROR_TYPE_NONE = 0,
	USB_ERROR_TYPE_STRING,
	USB_ERROR_TYPE_ERRNO
} usb_error_type_t;
static  char usb_error_string[1024];
static  int usb_error_errno;
static  usb_error_type_t usb_error_type = USB_ERROR_TYPE_NONE;

static void
wr_error_str(int x, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	usb_error_type = USB_ERROR_TYPE_STRING;
	usb_error_errno = x;

	(void) vsnprintf(usb_error_string, sizeof (usb_error_string),
			 format, ap);

	usbi_debug(NULL, 4, "USB error (%d): %s\n", x, usb_error_string);

	va_end(ap);
}

static int
wr_error(int x)
{
	usbi_debug(NULL, 4, "usb_error(): error=%d\n", x);

	usb_error_type = USB_ERROR_TYPE_ERRNO;
	usb_error_errno = x;

	return (-x);
}

char *usb_strerror(void)
{
	usbi_debug(NULL, 4, "usb_strerror(): "
		"usb_error_type=%d, errno=%d\n", usb_error_type,
		usb_error_errno);

	switch (usb_error_type) {
		case USB_ERROR_TYPE_NONE:
			return ("No error");

		case USB_ERROR_TYPE_STRING:
			return (usb_error_string);

		case USB_ERROR_TYPE_ERRNO:
			if (usb_error_errno > 0) {

				return (strerror(usb_error_errno));
			}
		default:
			break;
	}

	return ("Unknown error");
}
/* error strings end */

void usb_init(void)
{
	int ret;

	ret = openusb_init(0, &wr_handle);
	if (ret < 0) {
		usbi_debug(NULL, 1, "fail");

		wr_error_str(ret,"usb_init fail");
		return;
	}
}

/* no corresponding openusb_fini entry in 0.1 !! */
void usb_fini(void)
{
}

/* set debug level */
void usb_set_debug(int level)
{
	openusb_set_debug(wr_handle, level, 0, NULL);
}

/*
 * some 0.1.x applications, e.g.coldsync, can't recognize multiple buses.
 * To make such applications still working without modification, we keep
 * single bus
 */
int usb_find_busses(void)
{
	/* do nothing, bus already initialized in usbi_init_common() */	

	/* need to map internal bus to 0.1 exported bus */
	struct usb_bus *bus;
	
	if (usb_busses != NULL) {
	/* already initialized */
		return 0;
	}

	if ((bus = calloc(sizeof(*bus), 1)) == NULL) {
		return(wr_error(ENOMEM));
	}

#ifdef SUNOS_API
	strncpy(bus->dirname, "/dev/usb/", sizeof(bus->dirname));
#endif
	usb_busses = bus;

	return 1;
}

static int wr_create_devices(struct usb_bus *bus, struct usbi_bus *ibus) 
{
	struct usbi_device *idev, *tdev;
	struct usb_device *dev, *ndev;
	int dev_cnt = 0;
	struct usb_dev_handle *tmph;

	pthread_mutex_lock(&ibus->lock);
	list_for_each_entry_safe(idev, tdev, &ibus->devices.head, bus_list) {
		if (idev) {
			dev = calloc(sizeof(*dev), 1);
			if (!dev) {
				wr_error_str(errno, "create_devices: No memory");
				pthread_mutex_unlock(&ibus->lock);
				return -1;
			}
			memcpy(dev->filename, idev->sys_path, PATH_MAX);
			dev->bus = bus;
	
			dev->config = NULL; 
			dev->dev = NULL;
	
			pthread_mutex_unlock(&ibus->lock);
			tmph = usb_open(dev); /* device descriptors will be filled */
			pthread_mutex_lock(&ibus->lock);
	
			if (!tmph) {
			/* if it can't be opened, don't add it to device list */
				continue;
			}
	
			pthread_mutex_unlock(&ibus->lock);
			usb_close(tmph); /* descriptors get set up */
			pthread_mutex_lock(&ibus->lock);
			
			/* add this device to the bus's device list */
			if(bus->devices == NULL) {
				bus->devices = dev;
				usbi_debug(NULL, 4, "add device: %s",dev->filename);
			} else {
				ndev = bus->devices;
				while(ndev->next) {
					ndev = ndev->next;
				}
				ndev->next = dev;
				usbi_debug(NULL, 4, "add device: %s",dev->filename);
			}
	
			if(idev->parent != NULL) {
				/* don't account for root hubs */
				dev_cnt++;
			}
		}
	}

	pthread_mutex_unlock(&ibus->lock);

	return dev_cnt;
}

int usb_find_devices(void)
{
	struct usb_bus *bus;
	struct usbi_bus *ibus, *tbus;
	int dev_cnt=0;
	int ret;

	pthread_mutex_lock(&usbi_buses.lock);	
	bus = usb_busses;

	while(bus) {

		list_for_each_entry_safe(ibus, tbus, &usbi_buses.head, list) {
			if (ibus) {

				pthread_mutex_unlock(&usbi_buses.lock);	

				ret = wr_create_devices(bus, ibus);

				pthread_mutex_lock(&usbi_buses.lock);	

				if (ret >= 0) {
					dev_cnt += ret;
				} else {
					usbi_debug(NULL, 1,
							"create_device error");
					wr_error_str(1,
							"wr_create_device error");
	
					pthread_mutex_unlock(&usbi_buses.lock);	
					return -1;
				}
			}
		}

		usbi_debug(NULL, 1, "bus: %s", bus->dirname);
		bus = bus->next;
	}

	pthread_mutex_unlock(&usbi_buses.lock);	
	return(dev_cnt);
}

/*
 * given a usb_device, find a corresponding device in openusb1.0
 * return the devid of openusb1.0
 */
static openusb_devid_t wr_find_device(struct usb_device *dev)
{
	openusb_devid_t devid = -1;
	struct usbi_bus *ibus;
	struct usbi_device *idev = NULL;
	int found = 0;
	
	pthread_mutex_lock(&usbi_buses.lock);
	list_for_each_entry(ibus, &usbi_buses.head, list) {
	/* safe */
		if (ibus) {
			pthread_mutex_lock(&ibus->devices.lock);
			list_for_each_entry(idev, &ibus->devices.head, bus_list) {
			/* safe */
				if (strncmp(idev->sys_path, dev->filename,
			    	PATH_MAX) == 0) {
	
					found = 1;
					pthread_mutex_unlock(&ibus->devices.lock);
	
					goto out;
				}
			}
			pthread_mutex_unlock(&ibus->devices.lock);
		}
	}
out:
	if (found==1) {
		devid = idev->devid;
	}

	pthread_mutex_unlock(&usbi_buses.lock);

	return devid;
}

struct usb_dev_handle_internal {
	struct usb_device *dev;
	openusb_devid_t devid;
	openusb_dev_handle_t devh;

	int config;
	int interface;
	int alt;
};

static int wr_parse_endpoint(struct usb_interface_descriptor *ifdesc,
	struct usbi_altsetting *alt)
{
	int num_eps;
	struct usbi_endpoint *ep10; /* 1.0 */
	struct usb_endpoint_descriptor *ep01; /* 0.1.x */
	int len;
	int i;

	num_eps = alt->num_endpoints;
	if (num_eps > 0) {
		len = sizeof(struct usb_endpoint_descriptor) * num_eps;
		ifdesc->endpoint = calloc(len, 1);
		if (!ifdesc->endpoint) {
			return -1;
		}
	} else {
		/* this interface may have no Endpoint */
		return 0;
	}
	
	for (i = 0; i < num_eps; i++) {
		ep01 = &ifdesc->endpoint[i];
		ep10 = &alt->endpoints[i];

		ep01->bLength = ep10->desc.bLength;
		ep01->bDescriptorType = ep10->desc.bDescriptorType;
		ep01->bEndpointAddress = ep10->desc.bEndpointAddress;
		ep01->bmAttributes = ep10->desc.bmAttributes;
		ep01->wMaxPacketSize =
			openusb_le32_to_cpu(ep10->desc.wMaxPacketSize);
		ep01->bInterval = ep10->desc.bInterval;
		ep01->bRefresh = ep10->desc.bRefresh;
		ep01->bSynchAddress = ep10->desc.bSynchAddress;

		if (ep10->extra) {
			ep01->extra = calloc(ep10->extralen, 1);
			if (!ep01->extra) {
				return -1;
			}
			memcpy(ep01->extra, ep10->extra, ep10->extralen);
			ep01->extralen = ep10->extralen;
		}
	}

	return 0;
}

static int wr_parse_interface(struct usb_interface * ifc01,
	struct usbi_interface *ifc10)
{
	int num_alts;
	struct usbi_altsetting *alt; /* 1.0 */
	struct usb_interface_descriptor *ifdesc;/* 0.1 */
	int i;

	num_alts = ifc10->num_altsettings;
	
	ifc01->altsetting = 
		calloc(sizeof(struct usb_interface_descriptor) * num_alts, 1);

	if (!ifc01->altsetting) {
		return -1;
	}

	ifc01->num_altsetting = num_alts;

	for (i = 0; i < num_alts; i++) {

		alt = &ifc10->altsettings[i];
		ifdesc = &ifc01->altsetting[i];

		ifdesc->bLength = alt->desc.bLength;
		ifdesc->bDescriptorType = alt->desc.bDescriptorType;
		ifdesc->bInterfaceNumber = alt->desc.bInterfaceNumber;
		ifdesc->bAlternateSetting = alt->desc.bAlternateSetting;
		ifdesc->bNumEndpoints = alt->desc.bNumEndpoints;
		ifdesc->bInterfaceClass = alt->desc.bInterfaceClass;
		ifdesc->bInterfaceSubClass = alt->desc.bInterfaceSubClass;
		ifdesc->bInterfaceProtocol = alt->desc.bInterfaceProtocol;
		ifdesc->iInterface = alt->desc.iInterface;

		if (alt->extra) {
			ifdesc->extra = calloc(alt->extralen, 1);
			if (!ifdesc->extra) {
				return -1;
			}
			memcpy(ifdesc->extra, alt->extra, alt->extralen);
			ifdesc->extralen = alt->extralen;
		}
		
		if (wr_parse_endpoint(ifdesc, alt) != 0) {
			free(ifc01->altsetting);
			return -1;
		}
	}

	return 0;
}

/*
 * the current implemetation just copy openusb1.0 cached descriptors to 0.1
 * device's descriptors. Since 1.0 doesn't cache Vendor/Class specific data,
 * the 0.1 extra of config,interface,endpoint is NULL. usbi_parse_configuration
 * should be enhanced to support extra data parsing.
 */
static int wr_setup_dev_config(struct usb_device *dev, openusb_devid_t devid,
	openusb_dev_handle_t devh)
{
	struct usbi_device *idev;
	struct usbi_descriptors *desc;
	int num_configs;
	int i;
	struct usb_config_descriptor *pcfg;
	usb_config_desc_t *picfg;
	struct usbi_config *config;
	int ret;
	struct usbi_dev_handle *hdev;

	hdev = usbi_find_dev_handle(devh);
	if (!hdev) {
		return -1;
	}
	
	/* get descriptors on the fly */
	ret = usbi_fetch_and_parse_descriptors(hdev);
	if (ret != 0) {
		usbi_debug(NULL, 1, "fail to get descriptor");
		return -1;
	}

	idev = usbi_find_device_by_id(devid);
	if (!idev) {
		usbi_debug(NULL, 1, "Can't find device %d",(int)devid);
		return -1;
	}
	
	desc = (struct usbi_descriptors*)&idev->desc;

	memcpy(&dev->descriptor, &desc->device, sizeof(usb_device_desc_t));

	num_configs = desc->device.bNumConfigurations;

	if (num_configs == 0) {
		usbi_debug(NULL, 1, "Zero configurations");
		dev->config = NULL;
		return -1;
	}

	dev->config = 
		calloc(sizeof (struct usb_config_descriptor) * num_configs, 1);

	if (!dev->config) {
		return OPENUSB_NO_RESOURCES;
	}

	for(i = 0; i < num_configs; i++) {
		int num_ifs;
		struct usbi_interface *ifc10;
		struct usb_interface *ifc01;
		int j;

		pcfg = (struct usb_config_descriptor *)&dev->config[i];	
		config = &idev->desc.configs[i];

		picfg = &config->desc;

		pcfg->bLength = picfg->bLength;
		pcfg->bDescriptorType = picfg->bDescriptorType;
		pcfg->wTotalLength = openusb_le32_to_cpu(picfg->wTotalLength);
		pcfg->bNumInterfaces = picfg->bNumInterfaces;
		pcfg->bConfigurationValue = picfg->bConfigurationValue;
		pcfg->iConfiguration = picfg->iConfiguration;
		pcfg->bmAttributes = picfg->bmAttributes;
		pcfg->MaxPower = picfg->bMaxPower;

		if (config->extralen) {
			pcfg->extra = calloc(config->extralen, 1);
			if (!pcfg->extra) {
				return -1;
			}
			memcpy(pcfg->extra, config->extra, config->extralen);
			pcfg->extralen = config->extralen;
		}

		/* begin build up interfaces */
		num_ifs = config->num_interfaces;
		if(num_ifs == 0) {
		/* zero interfaces ? */
			usbi_debug(NULL, 4, "Zero interfaces");
			return 0;
		}
		pcfg->interface = 
			calloc(sizeof(struct usb_interface) * num_ifs, 1);
		if (!pcfg->interface) {
			free(dev->config);
			return OPENUSB_NO_RESOURCES;
		}
		
		for(j = 0; j < num_ifs; j++) {
			ifc01 = &pcfg->interface[j];
			ifc10 = &config->interfaces[j];
			if (wr_parse_interface(ifc01, ifc10) != 0) {
				free(pcfg->interface);
				return -1;
			}
		}
	}

	return 0;
}

struct usb_dev_handle *usb_open(struct usb_device *dev)
{
	struct usb_dev_handle_internal *devh;
	openusb_dev_handle_t usb1_devh;
	int ret;
	openusb_devid_t devid;
	
	if (dev == NULL) {
		wr_error_str(EINVAL, "usb_open: invalid arguments");
		return NULL;
	}
	/* need to check this dev is on busses */
	
	devid = wr_find_device(dev);
	if (devid < 0) {
		wr_error_str(devid, "usb_open: invalid arguments");
		usbi_debug(NULL, 1, "No such device");
		return NULL;
	}

	ret = openusb_open_device(wr_handle, devid, 0, &usb1_devh);
	if (ret != 0) {
		usbi_debug(NULL, 1, "Fail to open device: %s",
				openusb_strerror(ret));
		wr_error_str(ret, "Fail to open device: %s",
			openusb_strerror(ret));

		return NULL;
	}

	/* shall we build up device descriptors on open ? */
	ret = wr_setup_dev_config(dev, devid, usb1_devh);
	if (ret != 0) {
		usbi_debug(NULL, 1, "Fail to set device config");
		return NULL;
	}


	devh = calloc(sizeof(struct usb_dev_handle_internal), 1);
	if (!devh) {
		wr_error(errno);
		openusb_close_device(usb1_devh);
		usb1_devh = 0;
		return NULL;
	}

	devh->dev = dev;
	devh->devid = devid;
	devh->devh = usb1_devh;

	return((struct usb_dev_handle*)devh);
}

int usb_close(struct usb_dev_handle *dev)
{
	struct usb_dev_handle_internal *devh;
	int ret;

	if (!dev) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}
	devh = (struct usb_dev_handle_internal *)dev;

	ret = openusb_close_device(devh->devh);
	devh->devh = 0;

	if (ret != 0) {
	/* shall we free the dev ? */
		wr_error_str(ret, "close_device fail");
		return ret;
	}

	free(devh);

	return(0);
}

/* internal bulk transfer function
 * throw all validity check work to openusb1.0 API
 */
static int usb0_bulk_xfer(struct usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
	openusb_bulk_request_t bulk;
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh || !bytes || size <= 0) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	memset(&bulk, 0, sizeof(bulk));
	bulk.payload = (unsigned char *)bytes;
	bulk.length = size;
	bulk.timeout = timeout;

	ret = openusb_bulk_xfer(devh->devh, devh->interface, ep, &bulk);

	if (ret < 0 || bulk.result.status != 0) {
		wr_error_str(ret, "bulk transfer fail");
		return -1;
	}

	/* not sure what 0.1 expect of this return value */
	return(bulk.result.transferred_bytes);
}

int usb_bulk_write(struct usb_dev_handle *dev, int ep, USBCONSTISSUE char *bytes, int size,
	int timeout)
{
	return (usb0_bulk_xfer(dev, ep, (char*)bytes, size, timeout));
}

int usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	return (usb0_bulk_xfer(dev, ep, bytes, size, timeout));
}

/* 
 * internal interrupt transfer function
 * throw all validity check work to openusb1.0 API
 */
static int usb0_intr_xfer(struct usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
	openusb_intr_request_t intr;
	int ret;
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	
	if (!devh || !bytes || size <= 0) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	memset(&intr, 0, sizeof(intr));
	intr.payload = (unsigned char *)bytes;
	intr.length = size;
	intr.timeout = timeout;

	ret = openusb_intr_xfer(devh->devh, devh->interface, ep, &intr);
	if (ret != 0 || intr.result.status != 0) {
		wr_error_str(ret, "interrupt transfer fail");
		return -1;
	}

	return(intr.result.transferred_bytes);
}

int usb_interrupt_write(usb_dev_handle *dev, int ep, USBCONSTISSUE char *bytes, int size,
	int timeout)
{
	return (usb0_intr_xfer(dev, ep, (char*)bytes, size, timeout));
}

int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	return (usb0_intr_xfer(dev, ep, bytes, size, timeout));
}

/* return -1, when status != 0 or ret < 0 */
int usb_control_msg(usb_dev_handle *dev, int requesttype, int request,
	int value, int index, char *bytes, int size, int timeout)
{
	openusb_ctrl_request_t ctrl;
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;
	
	if (!devh || size < 0) {
	/* NULL buffer is allowed for some CTRL xfer */
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	usbi_debug(NULL, 4, "type = %d, request=%d, index= %d",
		requesttype, request, index);

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.setup.bmRequestType = requesttype;
	ctrl.setup.bRequest = request;
	ctrl.setup.wValue = value;
	ctrl.setup.wIndex = index;

	ctrl.payload = (unsigned char *)bytes;
	ctrl.length = size;
	ctrl.timeout = timeout;

	ret = openusb_ctrl_xfer(devh->devh, 0, 0, &ctrl);

	if (ret < 0 || ctrl.result.status != 0) {
		wr_error_str(ret, "control transfer fail");
		return -1;
	}

	return (ctrl.result.transferred_bytes);
}


/*
 * openusb1.0 will check configuration validity
 */
int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
	struct usb_dev_handle_internal *devh;
	int ret;

	if (!dev) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	devh = (struct usb_dev_handle_internal *)dev;
	
	if ((ret = openusb_set_configuration(devh->devh,
		configuration)) == 0) {
		devh->config = configuration;
		return 0;
	}

	wr_error_str(ret, "set_configuration fail");
	return(ret);
}

/* openusb1.0 will check interface validity */
int usb_claim_interface(usb_dev_handle *dev, int interface)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	if ((ret = openusb_claim_interface(devh->devh,
		interface, 0)) == 0) {
		devh->interface = interface;
		return 0;
	}

	wr_error_str(ret, "set_configuration fail");
	return (ret);
}

/* openusb1.0 will check interface validity */
int usb_release_interface(usb_dev_handle *dev, int interface)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	ret = openusb_release_interface(devh->devh, interface);
	if (ret != 0) {
		wr_error_str(ret, "release_interface fail");
	}

	return ret;
}

/* openusb1.0 will check alternate validity */
int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}
	
	/* we throw validity check work to openusb1.0 APIs */
	if((ret = openusb_set_altsetting(devh->devh, devh->interface,
		alternate)) == 0) {
		devh->alt = alternate;
		return 0;
	}
	usbi_debug(NULL, 4, "openusb_set_altsetting error: %s",
		openusb_strerror(ret));

	wr_error_str(ret, "set_altinterface fail");
	return(ret);
}

/* dev is checked in usb_control_msg */
int usb_resetep(usb_dev_handle *dev, unsigned int ep)
{
	return (usb_clear_halt(dev,ep));
}

/*dev is checked in usb_control_msg */
int usb_clear_halt(usb_dev_handle *dev, unsigned int ep)
{
	int ret;

	/* need more check of ep */
	ret= usb_control_msg(dev,
		USB_REQ_HOST_TO_DEV | USB_RECIP_ENDPOINT,
		USB_REQ_CLEAR_FEATURE, 0, ep, NULL, 0, 0);
	
	if (ret != 0) {
		wr_error_str(ret, "clear_halt fail");
	}

	return ret;
}

int usb_reset(usb_dev_handle *dev)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	ret = openusb_reset(devh->devh);
	
	if(ret != 0) {
		wr_error_str(ret, "reset fail");
	}

	return(ret);
}

/* buf,buflen are checked in usbi_get_string */
int usb_get_string(usb_dev_handle *dev, int index, int langid,
	char *buf, size_t buflen)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;
	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	ret = usbi_get_string(devh->devh, index, langid, buf, buflen);

	if (ret != 0) {
		wr_error_str(ret, "get_string fail");
	}

	return(ret);
}

/* buf,buflen are checked in usbi_get_string */
int usb_get_string_simple(usb_dev_handle *dev, int index,
	char *buf, size_t buflen)
{
	struct usb_dev_handle_internal *devh = 
		(struct usb_dev_handle_internal *)dev;

	int ret;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return OPENUSB_BADARG;
	}
	
	ret = usbi_get_string_simple(devh->devh, index, buf, buflen);

	if (ret < 0) {
		wr_error_str(ret, "get_string_simple fail");
	}
	return ret;
}

int usb_get_descriptor_by_endpoint(usb_dev_handle *dev, int ep,
	uint8_t type, uint8_t index, void *buf, int size)
{
	int ret;

	if (!buf || size <= 0) {
		wr_error_str(EINVAL, "Invalid arguments");
		return OPENUSB_BADARG;
	}

	ret = usb_control_msg(dev,
			ep | USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
			(type << 8) + index, 0, buf, size, 1000);
	if (ret != 0) {
		wr_error_str(ret, "get_descriptor_by_endpoint fail");
	}

	return(ret);
}

int usb_get_descriptor(usb_dev_handle *dev, uint8_t type, uint8_t index,
	void *buf, int size)
{
	int ret;

	if (!buf || size <= 0) {
		wr_error_str(EINVAL, "Invalid arguments");
		return OPENUSB_BADARG;
	}

	ret = usb_control_msg(dev,
			USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
			(type << 8) + index, 0, buf, size, 1000);

	if (ret != 0) {
		wr_error_str(ret, "get_descriptor_by_endpoint fail");
	}

	return(ret);
}

struct usb_device *usb_device(usb_dev_handle *dev)
{
	struct usb_dev_handle_internal *devh =
		(struct usb_dev_handle_internal *)dev;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return NULL;
	}

	return devh->dev;
}

struct usb_bus *usb_get_busses(void)
{
	return(usb_busses);
}

int usb_get_driver_np(usb_dev_handle *dev, int interface, char *name,
											unsigned int namelen)
{
	struct usb_dev_handle_internal *devh =
			(struct usb_dev_handle_internal *)dev;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	return (usbi_get_driver_np(devh->devh, interface, name, namelen));
}


int usb_detach_kernel_driver_np(usb_dev_handle *dev, int interface)
{
	struct usb_dev_handle_internal *devh =
			(struct usb_dev_handle_internal *)dev;

	if (!devh) {
		wr_error_str(EINVAL, "Invalid arguments");
		return -1;
	}

	return (usbi_detach_kernel_driver_np(devh->devh, interface));
}
