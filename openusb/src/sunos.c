/* 
 * Solaris backend
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */
#include <stdlib.h>	/* getenv, etc */
#include <unistd.h>	/* read/write */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <libdevinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/asynch.h>
#include <fcntl.h>
#include <config_admin.h>
#include <pthread.h>
#include <sys/usb/clients/ugen/usb_ugen.h>

#include "usbi.h"
#include "sunos.h"

static int busnum = 0;
static di_devlink_handle_t devlink_hdl;

static pthread_t cb_thread;
static pthread_mutex_t cb_io_lock;
static pthread_cond_t cb_io_cond;
static struct list_head cb_ios = {.prev = &cb_ios, .next = &cb_ios};
static int32_t solaris_back_inited;

int solaris_get_raw_desc(struct usbi_device *idev,uint8_t type,uint8_t descidx,
                uint16_t langid,uint8_t **buffer, uint16_t *buflen);

/* fake root hub descriptors */
static usb_device_desc_t fs_root_hub_dev_descr = {
	0x12,   /* Length */
	1,      /* Type */
	0x110,  /* BCD - v1.1 */
	9,      /* Class */
	0,      /* Sub class */
	0,      /* Protocol */
	8,      /* Max pkt size */
	0,      /* Vendor */
	0,      /* Product id */
	0,      /* Device release */
	0,      /* Manufacturer */
	0,      /* Product */
	0,      /* Sn */
	1       /* No of configs */
};

static usb_device_desc_t hs_root_hub_dev_descr = {
	0x12,           /* bLength */
	0x01,           /* bDescriptorType, Device */
	0x200,          /* bcdUSB, v2.0 */
	0x09,           /* bDeviceClass */
	0x00,           /* bDeviceSubClass */
	0x01,           /* bDeviceProtocol */
	0x40,           /* bMaxPacketSize0 */
	0x00,           /* idVendor */
	0x00,           /* idProduct */
	0x00,           /* bcdDevice */
	0x00,           /* iManufacturer */
	0x00,           /* iProduct */
	0x00,           /* iSerialNumber */
	0x01            /* bNumConfigurations */
};

static uchar_t root_hub_config_descriptor[] = {
	/* One configuartion */
	0x09,           /* bLength */
	0x02,           /* bDescriptorType, Configuartion */
	0x19, 0x00,     /* wTotalLength */
	0x01,           /* bNumInterfaces */
	0x01,           /* bConfigurationValue */
	0x00,           /* iConfiguration */
	0x40,           /* bmAttributes */
	0x00,           /* MaxPower */

	/* One Interface */
	0x09,           /* bLength */
	0x04,           /* bDescriptorType, Interface */
	0x00,           /* bInterfaceNumber */
	0x00,           /* bAlternateSetting */
	0x01,           /* bNumEndpoints */
	0x09,           /* bInterfaceClass */
	0x01,           /* bInterfaceSubClass */
	0x00,           /* bInterfaceProtocol */
	0x00,           /* iInterface */

	/* One Endpoint (status change endpoint) */
	0x07,           /* bLength */
	0x05,           /* bDescriptorType, Endpoint */
	0x81,           /* bEndpointAddress */
	0x03,           /* bmAttributes */
	0x01, 0x00,     /* wMaxPacketSize, 1 +  (EHCI_MAX_RH_PORTS / 8) */
	0xff            /* bInterval */
};

struct ugen_lc2stat {
	int lc;
	int stat;
} ugen_lc2libstat_table[] = {
	{USB_LC_STAT_NOERROR,		OPENUSB_SUCCESS			},
	{USB_LC_STAT_CRC,		OPENUSB_IO_CRC_ERROR		},
	{USB_LC_STAT_DEV_NOT_RESP,	OPENUSB_IO_DEVICE_HUNG		},
	{USB_LC_STAT_BITSTUFFING,	OPENUSB_IO_BIT_STUFFING		},
	{USB_LC_STAT_UNEXP_PID,		OPENUSB_IO_UNEXPECTED_PID	},
	{USB_LC_STAT_DATA_OVERRUN,	OPENUSB_IO_DATA_OVERRUN		},
	{USB_LC_STAT_DATA_UNDERRUN,	OPENUSB_IO_DATA_UNDERRUN		},
	{USB_LC_STAT_BUFFER_OVERRUN,	OPENUSB_IO_BUFFER_OVERRUN	},
	{USB_LC_STAT_BUFFER_UNDERRUN,	OPENUSB_IO_BUFFER_UNDERRUN	},
	{USB_LC_STAT_PID_CHECKFAILURE,	OPENUSB_IO_PID_CHECK_FAILURE	},
	{USB_LC_STAT_DATA_TOGGLE_MM,	OPENUSB_IO_DATA_TOGGLE_MISMATCH	},
	{USB_LC_STAT_TIMEOUT,		OPENUSB_IO_TIMEOUT		},
	{USB_LC_STAT_STALL, 		OPENUSB_IO_STALL			}
};

static int ugen_lc2libstat(int lc)
{
	int i;
	for (i = 0; i < sizeof(ugen_lc2libstat_table); i++) {
		if (ugen_lc2libstat_table[i].lc == lc) {

			return (ugen_lc2libstat_table[i].stat);
		}
	}

	return OPENUSB_PLATFORM_FAILURE;
}

#define OPENUSB_USE_HAL
#ifdef OPENUSB_USE_HAL /* hal */
/*
 * Use HAL for USB device hotplug events monitoring.
 * But HAL introduces too many dependencies for OpenUSB. Though
 * we don't like it, we have to depends on it, unless we find a
 * better way. Enclose this code segment in a conditional compiling
 * flag.
 */

#include <glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <libhal.h>

static LibHalContext *hal_ctx;
static char *show_device = NULL;
GMainLoop *event_loop;

pthread_t hotplug_thread;

static int solaris_refresh_devices(struct usbi_bus *ibus);

struct usbi_device *find_device_by_syspath(char *path)
{
	struct usbi_device *idev;
	char newpath[PATH_MAX+1];
	

	snprintf(newpath, PATH_MAX, "/devices%s", path);

	list_for_each_entry(idev, &usbi_devices.head, dev_list) {
		if (!idev)
			break;

		if ((strcmp(path, idev->sys_path) == 0) ||
			strcmp(newpath, idev->sys_path) == 0) {
			return idev;
		}
	}

	return NULL;
}


struct usbi_device *find_device_by_udi(const char *udi)
{
	struct usbi_device *idev = NULL;
	
	usbi_debug(NULL, 4, "searching device: %s", udi);
	
	pthread_mutex_lock(&usbi_devices.lock);

	list_for_each_entry(idev, &usbi_devices.head, dev_list) {
		if (!idev->priv->udi) {
			continue;
		}

		if (strcmp(udi, idev->priv->udi) == 0) {
			pthread_mutex_unlock(&usbi_devices.lock);
			return idev;
		}
	}

	pthread_mutex_unlock(&usbi_devices.lock);

	return NULL;
}

static void
set_device_udi(void)
{
	int i;
	int num_devices;
	char **device_names;
	DBusError error;
	char *devpath;
	struct usbi_device *idev;

	usbi_debug(NULL, 1, "Begin");

	dbus_error_init (&error);

	device_names = libhal_get_all_devices (hal_ctx, &num_devices, &error);

	if (device_names == NULL) {
		LIBHAL_FREE_DBUS_ERROR (&error);
		usbi_debug(NULL, 1, "Couldn't obtain list of devices\n");
		return;
	}

	for (i = 0;i < num_devices;i++) {

		devpath = libhal_device_get_property_string (hal_ctx,
				device_names[i], "solaris.devfs_path", &error);

		if (dbus_error_is_set (&error)) {
			/* Free the error (which include a dbus_error_init())
			   This should prevent errors if a call above fails */
			usbi_debug(NULL, 4, "get device syspath error: %s",
				device_names[i]);
			dbus_error_free (&error);
			continue;
		}

		idev = find_device_by_syspath(devpath);
		if (idev) {
			usbi_debug(NULL, 4, "set udi: %s of device: %s",
				device_names[i], devpath);
			if (idev->priv->udi == NULL) {
			/*
			 * if the device is re-inserted before internal
			 * structure getting freed, its udi will be still
			 * valid
			 */
				idev->priv->udi = strdup(device_names[i]);
			}
		}
		
		libhal_free_string(devpath);
	}


	libhal_free_string_array (device_names);
}

void process_new_device(const char *udi)
{
	/* assign new device id,
	 * get device descripotr?
	 * fill device structures
	 * add a callback if ATTACH callback is set
	 */
	struct usbi_device  *pidev;
	char *parent;
	DBusError error;
	char *devpath;
	char *subsys;

	dbus_error_init(&error);

	devpath = libhal_device_get_property_string(hal_ctx,
			udi, "solaris.devfs_path", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 2, "Get devfs_path fail");
		dbus_error_free(&error);
		return;
	}

	subsys = libhal_device_get_property_string(hal_ctx,
			udi, "info.subsystem", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 2, "Get info.subsystem fail");
		libhal_free_string(devpath);
		dbus_error_free(&error);
		return;
	}
	
	usbi_debug(NULL, 4, "subsys = %s", subsys);

	if (strcmp(subsys, "usb_device") != 0) {
	/* we only care usb device */
		usbi_debug(NULL, 4, "subsys = %s", subsys);
		libhal_free_string(subsys);
		dbus_error_free(&error);
		return;
	}

	parent = libhal_device_get_property_string(hal_ctx, udi,
		"info.parent", &error);
	if (dbus_error_is_set(&error)) {
		libhal_free_string(subsys);
		libhal_free_string(devpath);
		dbus_error_free(&error);
		return;
	}

	usbi_debug(NULL, 4, "parent: %s", parent);
	
	pidev = find_device_by_udi(parent); /* get parent's usbi_device */
	if (!pidev) {
		usbi_debug(NULL, 4, "Can't find parent's usbi_device");
		goto add_fail;
	}

	/*
	 * get descriptors, add device to global list,
	 * add event callback. Refresh device list on the bus. 
	 * This func can simplify such case when a hub is hotplugged.
	 */
	solaris_refresh_devices(pidev->bus);

add_fail:
	libhal_free_string(parent);
	libhal_free_string(devpath);
	libhal_free_string(subsys);
}

/*
 * Invoked when a device is added to the Global Device List of HAL.
 *
 */
static void
device_added (LibHalContext *ctx, const char *udi)
{
	struct usbi_device *idev = NULL;
	struct usbi_handle *handle, *thdl;

	usbi_debug(NULL, 4, "Event: device_added, udi='%s'", udi);

	idev = find_device_by_udi(udi);
	if (idev) {
		/* old device re-inserted */
		usbi_debug(NULL, 4, "old device: %d", (int)idev->devid);
		pthread_mutex_lock(&usbi_handles.lock);
		list_for_each_entry_safe(handle, thdl, &usbi_handles.head,
			list) {
			/* every openusb instance should get notification
			 * of this event
			 */
			usbi_add_event_callback(handle, idev->devid,
				USB_ATTACH);
		}
		pthread_mutex_unlock(&usbi_handles.lock);

	} else {
		usbi_debug(NULL, 4, "new device");
		process_new_device(udi);
		set_device_udi();
	}
}


/*
 * Invoked when a device is removed from the Global Device List of HAL.
 *
 */
static void
device_removed (LibHalContext *ctx, const char *udi)
{
	struct usbi_device *idev = NULL;
	struct usbi_handle *hdl;

	usbi_debug(NULL, 4, "Event: device_removed, udi='%s'", udi);

	idev = find_device_by_udi(udi);

	if (idev) {
		/* add a callback if REMOVE callback is set */ 
		pthread_mutex_lock(&usbi_handles.lock);
		list_for_each_entry(hdl, &usbi_handles.head, list) {
			pthread_mutex_unlock(&usbi_handles.lock);
			usbi_add_event_callback(hdl, idev->devid, USB_REMOVE);
			pthread_mutex_lock(&usbi_handles.lock);
		}
		pthread_mutex_unlock(&usbi_handles.lock);

	} else {
		/* we don't care */
	}
}



/*
 * hotplug event monitoring thread
 */
int
hal_hotplug_event_thread(void)
{
	DBusError error;
	DBusConnection *conn;
	GMainContext *context;
	
	usbi_debug(NULL, 4, "start hotplug thread");

	context = g_main_context_new();
	event_loop = g_main_loop_new (context, FALSE);

	dbus_error_init (&error);
	conn = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		fprintf (stderr, "error: dbus_bus_get: %s: %s\n",
				error.name, error.message);
		LIBHAL_FREE_DBUS_ERROR (&error);
		return 1;
	}

	dbus_connection_setup_with_g_main (conn, context);

	if ((hal_ctx = libhal_ctx_new ()) == NULL) {
		fprintf (stderr, "error: libhal_ctx_new\n");
		return 1;
	}
	if (!libhal_ctx_set_dbus_connection (hal_ctx, conn)) {
		fprintf (stderr,
			"error: libhal_ctx_set_dbus_connection: %s: %s\n",
				error.name, error.message);
		return 1;
	}
	if (!libhal_ctx_init (hal_ctx, &error)) {
		if (dbus_error_is_set(&error)) {
			fprintf (stderr, "error: libhal_ctx_init: %s: %s\n",
			error.name, error.message);
			LIBHAL_FREE_DBUS_ERROR (&error);
		}
		fprintf (stderr, "Could not initialise connection to hald.\n"
				"Normally this means the HAL daemon (hald) is"
				"not running or not ready.\n");
		return 1;
	}

	set_device_udi(); /* set and initialize all devices' udi */
	sleep(1); /* wait a while till all device udi being set */

	libhal_ctx_set_device_added (hal_ctx, device_added);
	libhal_ctx_set_device_removed (hal_ctx, device_removed);

	/* run the main loop */
	if (event_loop != NULL) {

		usbi_debug(NULL, 4, "hotplug thread running");
		g_main_loop_run (event_loop);

	}

	if (libhal_ctx_shutdown (hal_ctx, &error) == FALSE)
		LIBHAL_FREE_DBUS_ERROR (&error);
	libhal_ctx_free (hal_ctx);

	dbus_connection_close (conn);
	dbus_connection_unref (conn);
	
	g_main_context_unref(context);
	g_main_context_release(context);

	if (show_device)
		free(show_device);

	return 0;
}
#endif /* OPENUSB_USE_HAL */


/*
 * check this opened device's stat, every opened device will have
 * one thread polling its devstat
 */
static int solaris_poll_devstat(void *arg)
{
	struct usbi_dev_handle *hdev=(struct usbi_dev_handle *)arg;
	struct usbi_device *idev;
	int statfd;
	char ugendevstat[PATH_MAX];
	int status;
	struct pollfd fds[2];
	
	if (!hdev) {
		return 0;
	}

	idev = hdev->idev;

	snprintf(ugendevstat, PATH_MAX, "%s/devstat", idev->priv->ugenpath);
	usbi_debug(NULL, 4, "Poll devstat: %s",ugendevstat);

	statfd = open(ugendevstat,O_RDONLY);
	if(statfd < 0) {
		return(errno); /* errno may not reflect the real error */
	}

	/* remember it, so that we can close this device upon thread_cancel */
	hdev->priv->statfd = statfd;

	fds[0].fd = statfd;
	fds[0].events = POLLIN;
	while(1) {
		poll(fds, 1, -1);
		
		if(fds[0].revents & POLLIN) {
			if (read(statfd, &status, sizeof(status)) 
				!= sizeof(status)) {
				close(statfd);
				return(OPENUSB_SYS_FUNC_FAILURE);
			}
			switch(status) {
				case USB_DEV_STAT_DISCONNECTED: 
				/*device is disconnected */
					usbi_debug(NULL, 4, "STAT_DISCONNECT");
					usbi_add_event_callback(hdev->lib_hdl,
						idev->devid, USB_REMOVE);

					close(statfd);
					return 0;
				case USB_DEV_STAT_RESUMED:
				/*device is resumed */
				case USB_DEV_STAT_UNAVAILABLE:
					usbi_add_event_callback(hdev->lib_hdl,
						idev->devid, USB_RESUME);

					close(statfd);
					return 0;
				default:
					break;
			}
		}
	}

	close(statfd);
}

/*
 * polling an opened device's state
 */
static int solaris_create_polling_thread(struct usbi_dev_handle *hdev)
{
	int ret;

	ret = pthread_create(&hdev->priv->pollthr, NULL,
		(void *)solaris_poll_devstat, (void*)hdev);
	
	if (ret != 0) {
		usbi_debug(hdev->lib_hdl, 1, "pthread_create fail");
	}

	return ret;
}

/*
 * handle timeout of IO request on this device
 */
static int solaris_create_timeout_thread(struct usbi_dev_handle *hdev)
{
	int ret;

	ret = pthread_create(&hdev->priv->timeout_thr, NULL,
		(void *)timeout_thread, (void*)hdev);
	
	if (ret != 0) {
		usbi_debug(hdev->lib_hdl, 1, "pthread_create fail");
	}

	return ret;

}


/*
 * Get device descriptors through libdevinfo, instead of by reading
 * control endpoint. This is for cases where a device doesn't export
 * a ugen node.
 */
int solaris_get_raw_desc(struct usbi_device *idev,uint8_t type,uint8_t descidx,
                uint16_t langid,uint8_t **buffer, uint16_t *buflen)
{
	struct usbi_device *pdev;
	char *descrp;
	uint8_t model;
	usb_device_desc_t *pd;

	int ret;
	di_node_t devnode;
	int proplen;
	unsigned char *rdata;
	char *path;
	unsigned char *prop_data;

	if ((pdev = idev->parent) == NULL) {
		usbi_debug(NULL, 4, "Null parent, root hub");

		model = idev->bus->priv->model;
		/* should we return failure or success ? */
		if(type == USB_DESC_TYPE_DEVICE) {
			*buflen = sizeof(hs_root_hub_dev_descr);
			
			*buffer = malloc(*buflen);
			if (*buffer == NULL) {
				return OPENUSB_NO_RESOURCES;
			}

			if (model == SUNOS_BUS_EHCI) {
				memcpy(*buffer, &hs_root_hub_dev_descr,
					*buflen);
			} else {
				memcpy(*buffer, &fs_root_hub_dev_descr,
					*buflen);
			}
		} else if (type == USB_DESC_TYPE_CONFIG) {
			*buflen = sizeof(root_hub_config_descriptor);

			*buffer = malloc(*buflen);
			if (*buffer == NULL) {
				return OPENUSB_NO_RESOURCES;
			}

			memcpy(*buffer, &root_hub_config_descriptor,
				*buflen);
		} else {
			return (OPENUSB_PLATFORM_FAILURE);
		}

		return 0;
	}

	if ((strlen(idev->sys_path)) == 0) {
		usbi_debug(NULL, 4, "NULL path");
		return (OPENUSB_BADARG);
	}
	
	path = idev->sys_path + 8; /* skip leading /devices */

	usbi_debug(NULL, 4, "init node for %s", pdev->sys_path);

	devnode = di_init(path, DINFOCPYALL);
	if (devnode == DI_NODE_NIL) {
		usbi_debug(NULL, 4, "Can't init node for %s", idev->sys_path);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	if (di_prop_lookup_bytes(DDI_DEV_T_ANY, devnode, "root-hub",
		&prop_data) == 0) {

		return (DI_WALK_CONTINUE);
	}

	ret = OPENUSB_SUCCESS;

	switch (type) {
	case USB_DESC_TYPE_DEVICE:
		proplen = di_prop_lookup_bytes(DDI_DEV_T_ANY, devnode,
			"usb-dev-descriptor", &rdata);
		if (proplen <= 0) {
			usbi_debug(NULL, 4,
				"Fail to get usb-dev-descriptor property"
				"for %s(%s)",
				path, strerror(errno));

			di_fini(devnode);
			return (OPENUSB_SYS_FUNC_FAILURE);
		}

		pd = (usb_device_desc_t *)rdata;
		descrp = calloc(1, USBI_DEVICE_DESC_SIZE);
		if (!descrp) {
			usbi_debug(NULL, 1, "unable to allocate memory"
					" for device descriptor");

			di_fini(devnode);
			return OPENUSB_NO_RESOURCES;
		}

		/*
		 * usb dev descriptor is parsed in kernel.
		 * It has to be converted back to raw data.
		 */
		pd->bcdUSB = openusb_cpu_to_le16(pd->bcdUSB);
		pd->idVendor = openusb_cpu_to_le16(pd->idVendor);
		pd->idProduct = openusb_cpu_to_le16(pd->idProduct);
		pd->bcdDevice = openusb_cpu_to_le16(pd->bcdDevice);

		memcpy(descrp, pd, USBI_DEVICE_DESC_SIZE);

		*buffer = (char *)descrp;
		*buflen = USBI_DEVICE_DESC_SIZE;	

		break;

	case USB_DESC_TYPE_CONFIG:

		proplen = di_prop_lookup_bytes(DDI_DEV_T_ANY, devnode,
			"usb-raw-cfg-descriptors", &rdata);
		if (proplen <= 0) {
			usbi_debug(NULL, 4,
				"Fail to get usb-raw-cfg-descriptor"
				" property for %s(%s)",
				path, strerror(errno));

			di_fini(devnode);
			return (OPENUSB_SYS_FUNC_FAILURE);
		}

		descrp = calloc(1, proplen);
		if (!descrp) {
			usbi_debug(NULL, 1, "unable to allocate memory"
					" for device descriptor");

			di_fini(devnode);
			return OPENUSB_NO_RESOURCES;
		}
		memcpy(descrp, rdata, proplen);
		*buffer = (char *)descrp;
		*buflen = proplen;

		break;
	default:
		usbi_debug(NULL, 4, "Unknown descriptor type");
		ret = OPENUSB_PLATFORM_FAILURE;
	}
	
	di_fini(devnode);

	return (ret);

}

/*
 * XXX: The minor nodes and devlink information will not be available
 * once the debug kernel unloads a driver module. We need to force
 * load usb drivers esp. ugen driver.
 */
static int
check_devlink(di_devlink_t link, void *arg)
{
	struct devlink_cbarg *dlarg = (struct devlink_cbarg *)arg;
	const char *str;
	char *newstr, *p;

	usbi_debug(NULL, 4, "Minor node type: %s",
		di_minor_nodetype(dlarg->minor));

	if ((dlarg->idev->priv->devlink != NULL) &&
	    (dlarg->idev->priv->ugenpath != NULL) &&
	    (dlarg->idev->priv->ap_ancestry != NULL)) {

		return (DI_WALK_TERMINATE);
	}

	str = di_devlink_path(link);

	if ((strncmp("/dev/usb/", str, 9) != 0) &&
	    (strncmp("/dev/cfg/", str, 9) != 0)) {

		return (DI_WALK_CONTINUE);
	}
	
	/* check the minor node type */
	if (strcmp("ddi_ctl:attachment_point:usb",
	    di_minor_nodetype(dlarg->minor)) == 0) {
		/* cfgadm node */
		if (dlarg->idev->priv->ap_ancestry != NULL) {

			return (DI_WALK_CONTINUE);
		}

		if ((p = malloc(APID_NAMELEN + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, APID_NAMELEN + 1);
		str = di_minor_name(dlarg->minor);
		dlarg->idev->priv->ap_ancestry = p;

		/* retrieve cfgadm ap_id ancestry */
		if ((newstr = strrchr(str, '.')) != NULL) {
			(void) strncpy(p, str,
			    strlen(str) - strlen(newstr) + 1);
		}

		usbi_debug(NULL, 4, "ap_ancestry: %s",
			dlarg->idev->priv->ap_ancestry);

		return (DI_WALK_CONTINUE);
	} else if (strcmp("ddi_generic:usb",
	    di_minor_nodetype(dlarg->minor)) == 0) {
		/* ugen node */
		if (dlarg->idev->priv->ugenpath != NULL) {

			return (DI_WALK_CONTINUE);
		}

		if ((p = malloc(PATH_MAX + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		/* retrieve ugen link path */
		if ((newstr = strrchr(str, '/')) == NULL) {
			free(p);

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, PATH_MAX + 1);
		(void) strncpy(p, str, strlen(str) - strlen(newstr));
		dlarg->idev->priv->ugenpath = p;
		usbi_debug(NULL, 4, "ugen_link: %s", dlarg->idev->priv->ugenpath);

		return (DI_WALK_CONTINUE);
	} else {
		/* there should be only one such link */
		if ((p = malloc(PATH_MAX + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, PATH_MAX + 1);
		(void) strcpy(p, str);
		dlarg->idev->priv->devlink = p;
		usbi_debug(NULL, 4, "dev_link: %s", dlarg->idev->priv->devlink);

		return (DI_WALK_CONTINUE);
	}
}


static void
get_minor_node_link(di_node_t node, struct usbi_device *idev)
{
	di_minor_t minor = DI_MINOR_NIL;
	char *minor_path;
	struct devlink_cbarg arg;

	while ((minor = di_minor_next(node, minor)) != DI_MINOR_NIL) {
		minor_path = di_devfs_minor_path(minor);
		arg.idev = idev;
		arg.minor = minor;
		(void) di_devlink_walk(devlink_hdl, NULL, minor_path,
		    DI_PRIMARY_LINK, (void *)&arg, check_devlink);
		di_devfs_path_free(minor_path);
	}
}

/*
 * start from node, recursively set up device info
 */
static void
create_new_device(di_node_t node, struct usbi_device *pdev,
	struct usbi_bus *ibus)
{
	di_node_t cnode;
	struct usbi_device *idev;

	struct usbi_device *tmpdev = NULL;

	int *nport_prop, *port_prop, *addr_prop, n;
	char *phys_path;
	
	usbi_debug(NULL,4, "check %s%d", di_driver_name(node),
		di_instance(node));

	phys_path = di_devfs_path(node);

#if 1
	usbi_debug(NULL, 4, "device path: %s", phys_path);
	tmpdev = find_device_by_syspath(phys_path);
	if (tmpdev) {
		/* 
		 * this device was already there, use the old structure
		 * But refresh its data.
		 */
		usbi_debug(NULL, 4, "an old device already there");
		idev = tmpdev;
	} else { 
		/* new device */
#endif
		idev = (struct usbi_device *)malloc(sizeof (struct usbi_device));
		if (idev == NULL)
			return;

		memset(idev, 0, sizeof (struct usbi_device));

		idev->priv = calloc(sizeof(struct usbi_dev_private), 1);
		if (!idev->priv) {
			free(idev);
			return;
		}

		list_init(&idev->dev_list);
		list_init(&idev->bus_list);
	}

	if (node == ibus->priv->node) { /* root node */
		usbi_debug(NULL, 4, "root node");

		idev->devnum = 1;
		idev->parent = NULL;
		idev->found = 1;
	} else {
		n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
		    "assigned-address", &addr_prop);
		if ((n != 1) || (*addr_prop == 0)) {
			usbi_debug(NULL, 1, "cannot get valid usb_addr");
			free(idev);

			return;
		}

		n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
		    "reg", &port_prop);
		
		if ((n != 1) || (*port_prop > pdev->nports) ||
		   (*port_prop <= 0)) {
			usbi_debug(NULL, 1, "cannot get valid port index");
			free(idev);

			return;
		}
		

		idev->devnum = *addr_prop;
		idev->parent = pdev;
		idev->pport = *port_prop;
	}

	if ((n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
	    "usb-port-count", &nport_prop)) > 1) {
		usbi_debug(NULL, 1, "invalid usb-port-number");
		free(idev);

		return;
	}

	if (n == 1) {
		idev->nports = *nport_prop;

		if (!tmpdev) { /* new device */
			idev->children = malloc(idev->nports *
					sizeof (idev->children[0]));
		}
		if (idev->children == NULL) {
			free(idev);

			return;
		}

		memset(idev->children, 0, idev->nports *
		    sizeof (idev->children[0]));
	} else {
		idev->nports = 0;
	}

	snprintf(idev->sys_path, sizeof (idev->sys_path), "/devices%s",
	    phys_path);

	di_devfs_path_free(phys_path);

	get_minor_node_link(node, idev);

	usbi_debug(NULL, 4, "priv node: %s", di_devfs_path(ibus->priv->node));


	if (node == ibus->priv->node) {
		ibus->root = idev;
	} else {
		pdev->children[*port_prop-1] = idev;
	}
	
	if (!tmpdev) {
	/* new device only */
		usbi_add_device(ibus, idev);
	}

	idev->found = 1;
	idev->priv->info.ep0_fd = -1;
	idev->priv->info.ep0_fd_stat = -1;

	usbi_debug(NULL, 4, "found usb device: bus %d dev %d",
	    ibus->busnum, idev->devnum);
	usbi_debug(NULL, 4, "device path: %s", idev->sys_path);

	if (idev->nports) {
		cnode = di_child_node(node);
		while (cnode != DI_NODE_NIL) {
			create_new_device(cnode, idev, ibus);
			cnode = di_sibling_node(cnode);
		}
	}
}

static int
solaris_refresh_devices(struct usbi_bus *ibus)
{
	struct usbi_device *idev, *tidev;
	di_node_t root_node;

	usbi_debug(NULL, 4, "Begin:%p %s", ibus, ibus->sys_path);
	/* Search devices from root-hub */
	if ((root_node = di_init(ibus->sys_path, DINFOCPYALL)) ==
	    DI_NODE_NIL) {
		usbi_debug(NULL, 1, "di_init() failed: %s", strerror(errno));

		return (OPENUSB_PLATFORM_FAILURE);
	}

	if ((devlink_hdl = di_devlink_init(NULL, 0)) == NULL) {
		usbi_debug(NULL, 1, "di_devlink_init() failed: %s",
		    strerror(errno));
		di_fini(root_node);

		return (OPENUSB_PLATFORM_FAILURE);
	}

	pthread_mutex_lock(&ibus->devices.lock);

	/* Reset the found flag for all devices */
	list_for_each_entry(idev, &ibus->devices.head, bus_list) {
	/* safe */
		idev->found = 0;
	}

	ibus->priv->node = root_node;

	usbi_debug(NULL, 4, "root node: %s", di_devfs_path(root_node));
	create_new_device(root_node, NULL, ibus);

	list_for_each_entry_safe(idev, tidev, &ibus->devices.head, bus_list) {
		if (!idev->found) {
			/* Device disappeared, remove it */
			usbi_debug(NULL, 3, "device %d removed", idev->devnum);
			usbi_remove_device(idev);
		}
	}

	pthread_mutex_unlock(&ibus->devices.lock);

	di_fini(root_node);
	(void) di_devlink_fini(&devlink_hdl);

	return (OPENUSB_SUCCESS);
}

void sunos_set_max_xfer_size(struct usbi_bus *ibus, uint8_t rhmodel)
{
	switch(rhmodel) {
	case SUNOS_BUS_EHCI:
		ibus->max_xfer_size[USB_TYPE_CONTROL] = 0x10000; /* 64K */
		ibus->max_xfer_size[USB_TYPE_BULK] = 0xA0000; /* 640K */
		ibus->max_xfer_size[USB_TYPE_INTERRUPT] = 0x5000; /* 20K */

		/* FIXME: isoc is limited by 1024*ep.wMaxPacketSize */
		ibus->max_xfer_size[USB_TYPE_ISOCHRONOUS] = 1024 * 3072;
		break;
	case SUNOS_BUS_OHCI:
		ibus->max_xfer_size[USB_TYPE_CONTROL] = 0x10000; /* 64K */
		ibus->max_xfer_size[USB_TYPE_BULK] = 0x40000; /* 256K */
		ibus->max_xfer_size[USB_TYPE_INTERRUPT] = 0x2000; /* 8K */

		/* FIXME: isoc is limited by 256*ep.wMaxPacketSize */
		ibus->max_xfer_size[USB_TYPE_ISOCHRONOUS] = 256 * 1024;
		break;
	case SUNOS_BUS_UHCI:
		ibus->max_xfer_size[USB_TYPE_CONTROL] = 0x10000; /* 64K */
		ibus->max_xfer_size[USB_TYPE_BULK] = 0x1F000; /* 124K */
		ibus->max_xfer_size[USB_TYPE_INTERRUPT] = 0x500; /* */

		/* FIXME: isoc is limited by 256*ep.wMaxPacketSize */
		ibus->max_xfer_size[USB_TYPE_ISOCHRONOUS] = 256 * 1024;
		break;
	default:
		break;
	}
}

static int
detect_root_hub(di_node_t node, void *arg)
{
	struct list_head *busses = (struct list_head *)arg;
	struct usbi_bus *ibus;
	uchar_t *prop_data = NULL;
	char *phys_path;
	
	char *model;
	uint8_t rhmodel = 0; /* root hub model */

	if (di_prop_lookup_bytes(DDI_DEV_T_ANY, node, "root-hub",
	    &prop_data) != 0) {

		return (DI_WALK_CONTINUE);
	}

	if (di_prop_lookup_strings(DDI_DEV_T_ANY, node, "model", &model) > 0) {

		usbi_debug(NULL, 4, "root-hub model: %s",model);

		if (strstr(model,"EHCI") != NULL) {
			rhmodel = SUNOS_BUS_EHCI;
		} else if (strstr(model, "OHCI") != NULL) {
			rhmodel = SUNOS_BUS_OHCI;
		} else if (strstr(model, "UHCI") != NULL) {
			rhmodel = SUNOS_BUS_UHCI;
		}
	}

	ibus = (struct usbi_bus *)malloc(sizeof(*ibus));

	if (ibus == NULL) {

		usbi_debug(NULL,1, "malloc ibus failed: %s", strerror(errno));

		return (DI_WALK_TERMINATE);
	}

	memset(ibus, 0, sizeof(*ibus));
	
	ibus->priv = (struct usbi_bus_private *)
			calloc(sizeof(struct usbi_bus_private), 1);
	if (!ibus->priv) {
		free(ibus);
		usbi_debug(NULL,1, "malloc ibus private failed: %s",
			strerror(errno));
		return (DI_WALK_TERMINATE);
	}

	pthread_mutex_init(&ibus->lock, NULL);
	pthread_mutex_init(&ibus->devices.lock,NULL);
	
	/* FIXME: everytime solaris_find_busses is called, all old buses will
	 *	be refreshed as a new bus. This is improper.
	 */
	ibus->busnum = ++busnum;
	phys_path = di_devfs_path(node);
	snprintf(ibus->sys_path, sizeof (ibus->sys_path), "%s", phys_path);
	di_devfs_path_free(phys_path);
	
	ibus->priv->model = rhmodel;
	sunos_set_max_xfer_size(ibus, rhmodel);

	list_add(&ibus->list, busses);

	usbi_debug(NULL, 4, "found bus %s%d:%s", di_driver_name(node),
	    di_instance(node), ibus->sys_path);

	return (DI_WALK_PRUNECHILD);
}

static int
solaris_find_busses(struct list_head *busses)
{
	di_node_t root_node;

	/* Search dev_info tree for root-hubs */
	if ((root_node = di_init("/", DINFOCPYALL)) == DI_NODE_NIL) {
		usbi_debug(NULL, 1, "di_init() failed: %s", strerror(errno));

		return (OPENUSB_PLATFORM_FAILURE);
	}

	if (di_walk_node(root_node, DI_WALK_SIBFIRST, busses,
	    detect_root_hub) == -1) {
		usbi_debug(NULL, 1, "di_walk_node() failed: %s",
			strerror(errno));
		di_fini(root_node);

		return (OPENUSB_PLATFORM_FAILURE);
	}

	usbi_debug(NULL, 4, "solaris_find_busses finished");

	di_fini(root_node);

	return (OPENUSB_SUCCESS);
}

static void
solaris_free_device(struct usbi_device *idev)
{
	if (idev->sys_path) {
		free(idev->priv->devlink);
	}
	
	if (idev->priv) {
		if (idev->priv->ugenpath) {
			free(idev->priv->ugenpath);
		}

		if (idev->priv->ap_ancestry) {
			free(idev->priv->ap_ancestry);
		}

		free(idev->priv);
	}
}

static int
usb_open_ep0(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;
	char filename[PATH_MAX + 1];

	if (idev->priv->info.ep0_fd >= 0) {
		idev->priv->info.ref_count++;
		hdev->priv->eps[0].datafd = idev->priv->info.ep0_fd;
		hdev->priv->eps[0].statfd = idev->priv->info.ep0_fd_stat;

		usbi_debug(NULL,3, "ep0 of dev: %s already opened",
			idev->sys_path);

		return (0);
	}

	snprintf(filename, PATH_MAX, "%s/cntrl0", idev->priv->ugenpath);
	usbi_debug(NULL, 4, "opening %s", filename);

	hdev->priv->eps[0].datafd = open(filename, O_RDWR);
	if (hdev->priv->eps[0].datafd < 0) {
		usbi_debug(NULL, 1, "open cntrl0 of dev: %s failed (%s)",
		    filename, strerror(errno));

		return OPENUSB_SYS_FUNC_FAILURE;
	}

	snprintf(filename, PATH_MAX, "%s/cntrl0stat", idev->priv->ugenpath);

	usbi_debug(NULL, 4, "opening %s", filename);

	hdev->priv->eps[0].statfd = open(filename, O_RDONLY);
	if (hdev->priv->eps[0].statfd < 0) {
		usbi_debug(NULL, 1, "open cntrl0stat of dev: %s failed (%d)",
		    idev->sys_path, errno);
		close(hdev->priv->eps[0].datafd);
		hdev->priv->eps[0].datafd = -1;

		return (-1);
	}

	/* allow sharing between multiple opens */
	idev->priv->info.ep0_fd = hdev->priv->eps[0].datafd;
	idev->priv->info.ep0_fd_stat = hdev->priv->eps[0].statfd;
	idev->priv->info.ref_count++;

	usbi_debug(NULL, 4, "ep0 opened: %d,%d",idev->priv->info.ep0_fd, 
		idev->priv->info.ep0_fd_stat);

	return (0);
}

static int
solaris_open(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;
	int i;
	
	if (idev->priv->ugenpath == NULL) {
		usbi_debug(NULL, 1,
			"open dev: %s not supported,ugen path NULL",
			idev->sys_path);

		return (OPENUSB_NOT_SUPPORTED);
	}
	
	hdev->priv = calloc(sizeof(struct usbi_dev_hdl_private), 1);
	if (!hdev->priv) {
		return (OPENUSB_NO_RESOURCES);
	}

	/* set all file descriptors to "closed" */
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		hdev->priv->eps[i].datafd = -1;
		hdev->priv->eps[i].statfd = -1;
		if (i > 0) {
			hdev->priv->ep_interface[i] = -1;
		}
		
	}

	hdev->config_value = 1;
	hdev->priv->config_index = -1;

	/* open default control ep and keep it open */
	if (usb_open_ep0(hdev) != 0) {

		return (OPENUSB_PLATFORM_FAILURE);
	}

#ifndef OPENUSB_USE_HAL  /* depends merely on HAL of hotplug events */
	if(solaris_create_polling_thread(hdev) != 0) {
		return OPENUSB_SYS_FUNC_FAILURE;
	}
#endif
	if(solaris_create_timeout_thread(hdev) != 0) {
		return OPENUSB_SYS_FUNC_FAILURE;
	}

	return (OPENUSB_SUCCESS);
}

static int
solaris_get_configuration(struct usbi_dev_handle *hdev, uint8_t *cfg)
{
	if(!hdev) {
		return (OPENUSB_NOT_SUPPORTED);
	}
	*cfg = hdev->config_value;

	return OPENUSB_SUCCESS;
}

static int
solaris_set_configuration(struct usbi_dev_handle *hdev, uint8_t cfg)
{
	/* may implement by cfgadm */
	if(!hdev) {
		return (OPENUSB_NOT_SUPPORTED);
	}

	hdev->config_value = cfg;
	hdev->priv->config_index = hdev->config_value - 1;

	return OPENUSB_SUCCESS;
}

#define USB_ENDPOINT_ADDRESS_MASK 0x0f
#define USB_ENDPOINT_DIR_MASK	0x80
/*
 * usb_ep_index:
 *	creates an index from endpoint address that can
 *	be used to index into endpoint lists
 *
 * Returns: ep index (a number between 0 and 31)
 */
static uchar_t
usb_ep_index(uint8_t ep_addr)
{
	return ((ep_addr & USB_ENDPOINT_ADDRESS_MASK) +
	    ((ep_addr & USB_ENDPOINT_DIR_MASK) ? 16 : 0));
}

/* initialize ep_interface arrays */
static void
usb_set_ep_iface_alts(struct usbi_dev_handle *hdev,
    int index, int interface, int alt)
{
	struct usbi_device *idev = hdev->idev;
	struct usbi_altsetting *as;
	struct usb_interface_desc *if_desc;
	struct usb_endpoint_desc *ep_desc;
	int i;
	
	usbi_debug(hdev->lib_hdl, 4, "Begin: idx=%d, ifc=%d, alt=%d",
		index, interface, alt);

	/* reinitialize endpoint arrays */
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		hdev->priv->ep_interface[i] = -1;	/* XXX: ep0? */
	}

	as = &idev->desc.configs[index].interfaces[interface].altsettings[alt];
	if_desc = &as->desc;

	usbi_debug(hdev->lib_hdl, 4, "bNumEP=%d",if_desc->bNumEndpoints);
	for (i = 0; i < if_desc->bNumEndpoints; i++) {
		ep_desc = (struct usb_endpoint_desc *)&as->endpoints[i];
		usbi_debug(hdev->lib_hdl, 4, "Address=%x",
			ep_desc->bEndpointAddress);

		hdev->priv->ep_interface[usb_ep_index(
		    ep_desc->bEndpointAddress)] = interface;
	}

	usbi_debug(hdev->lib_hdl, 3, "ep_interface:");
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		usbi_debug(hdev->lib_hdl, 3, "%d - %d ", i,
			hdev->priv->ep_interface[i]);
	}
}

static int
solaris_claim_interface(struct usbi_dev_handle *hdev, uint8_t interface, 
	openusb_init_flag_t flags)
{
	struct usbi_device *idev = hdev->idev;
	int index;

	index = hdev->config_value - 1;

	/* already claimed this interface */
	if (idev->priv->info.claimed_interfaces[interface] == hdev) {

		return (OPENUSB_SUCCESS);
	}

	/* interface claimed by others */
	if (idev->priv->info.claimed_interfaces[interface] != NULL) {
		usbi_debug(hdev->lib_hdl, 1,
			"this interface has been claimed by others");

		return (OPENUSB_BUSY);
	}

	/* claimed another interface */
	if (hdev->claimed_ifs[interface].clm != -1) {
		usbi_debug(hdev->lib_hdl, 1,
			"please release interface before claiming "
			"a new one");

		return (OPENUSB_BUSY);
	}

	hdev->claimed_ifs[interface].clm = USBI_IFC_CLAIMED;
	hdev->claimed_ifs[interface].altsetting = 0;
	
	idev->priv->info.claimed_interfaces[interface] = hdev;

	usb_set_ep_iface_alts(hdev, index, interface, 0);

	usbi_debug(hdev->lib_hdl, 4, "interface %d claimed", interface);

	return (OPENUSB_SUCCESS);
}

static int
solaris_release_interface(struct usbi_dev_handle *hdev, uint8_t interface)
{
	struct usbi_device *idev = hdev->idev;

	if ((hdev->claimed_ifs[interface].clm != USBI_IFC_CLAIMED)) {
		usbi_debug(hdev->lib_hdl, 1, "interface(%d) not claimed",
			interface);

		return (OPENUSB_BADARG);
	}

	if (idev->priv->info.claimed_interfaces[interface] != hdev) {
		usbi_debug(hdev->lib_hdl, 1, "interface not owned");

		return (OPENUSB_PLATFORM_FAILURE);
	}

	idev->priv->info.claimed_interfaces[interface] = NULL;
	hdev->claimed_ifs[interface].clm = -1;
	hdev->claimed_ifs[interface].altsetting = -1;

	return (OPENUSB_SUCCESS);
}

static void
usb_close_all_eps(struct usbi_dev_handle *hdev)
{
	int i;

	/* not close ep0 */

	for (i = 1; i < USBI_MAXENDPOINTS; i++) {
		if (hdev->priv->eps[i].datafd != -1) {
			(void) close(hdev->priv->eps[i].datafd);
			hdev->priv->eps[i].datafd = -1;
		}
		if (hdev->priv->eps[i].statfd != -1) {
			(void) close(hdev->priv->eps[i].statfd);
			hdev->priv->eps[i].statfd = -1;
		}
	}
}

static int
usb_close_ep0(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;

	if (idev->priv->info.ep0_fd >= 0) {
		if (--(idev->priv->info.ref_count) > 0) {
			usbi_debug(hdev->lib_hdl, 4,
				"ep0 of dev %s: ref_count=%d", idev->sys_path,
				idev->priv->info.ref_count);

			return (0);
		}

		if ((hdev->priv->eps[0].datafd != idev->priv->info.ep0_fd) ||
			(hdev->priv->eps[0].statfd !=
		    	idev->priv->info.ep0_fd_stat)) {
			usbi_debug(hdev->lib_hdl, 1,
				"unexpected error closing ep0 of dev %s",
				idev->sys_path);
			return (-1);
		}

		close(idev->priv->info.ep0_fd);
		close(idev->priv->info.ep0_fd_stat);
		idev->priv->info.ep0_fd = -1;
		idev->priv->info.ep0_fd_stat = -1;
		hdev->priv->eps[0].datafd = -1;
		hdev->priv->eps[0].statfd = -1;
		usbi_debug(hdev->lib_hdl, 4, "ep0 of dev %s closed",
			idev->sys_path);

		return (0);
	} else {
		usbi_debug(hdev->lib_hdl, 1,
			"ep0 of dev %s not open or already closed",
			idev->sys_path);

		return (-1);
	}
}

static int
solaris_close(struct usbi_dev_handle *hdev)
{
	int i;

#ifndef OPENUSB_USE_HAL /* depends on HAL */
	/* terminate all working threads of this handle */
	pthread_cancel(hdev->priv->pollthr);
	close(hdev->priv->statfd);
	pthread_join(hdev->priv->pollthr, NULL);
#endif

	pthread_cancel(hdev->priv->timeout_thr);

	/* wait for timeout thread exiting */
	pthread_join(hdev->priv->timeout_thr, NULL);

	pthread_mutex_lock(&hdev->lock);

	usbi_debug(hdev->lib_hdl, 4, "timeout thread exit");

	for(i = 0; i < USBI_MAXINTERFACES ; i++) {
		solaris_release_interface(hdev, i);
	}

	usb_close_all_eps(hdev);
	usb_close_ep0(hdev);
	
	hdev->state = USBI_DEVICE_CLOSING;
	free(hdev->priv);

	pthread_mutex_unlock(&hdev->lock);

	return (OPENUSB_SUCCESS);
}

static void
usb_dump_data(char *data, size_t size)
{
#if 0
	int i;

	(void) fprintf(stderr, "data dump:");
	for (i = 0; i < size; i++) {
		if (i % 16 == 0) {
			(void) fprintf(stderr, "\n%08x	", i);
		}
		(void) fprintf(stderr, "%02x ", (uchar_t)data[i]);
	}
	(void) fprintf(stderr, "\n");
#endif
}

/*
 * sunos_usb_get_status:
 *	gets status of endpoint
 *
 * Returns: ugen's last cmd status
 */
static int
sunos_usb_get_status(int fd)
{
	int status, ret;

	usbi_debug(NULL, 4, "sunos_usb_get_status(): fd=%d\n", fd);

	ret = read(fd, &status, sizeof (status));
	if (ret == sizeof (status)) {
		switch (status) {
		case USB_LC_STAT_NOERROR:
			usbi_debug(NULL, 4, "No Error\n");
			break;
		case USB_LC_STAT_CRC:
			usbi_debug(NULL, 1, "CRC Timeout Detected\n");
			break;
		case USB_LC_STAT_BITSTUFFING:
			usbi_debug(NULL, 1, "Bit Stuffing Violation\n");
			break;
		case USB_LC_STAT_DATA_TOGGLE_MM:
			usbi_debug(NULL, 1, "Data Toggle Mismatch\n");
			break;
		case USB_LC_STAT_STALL:
			usbi_debug(NULL, 1, "End Point Stalled\n");
			break;
		case USB_LC_STAT_DEV_NOT_RESP:
			usbi_debug(NULL, 1, "Device is Not Responding\n");
			break;
		case USB_LC_STAT_PID_CHECKFAILURE:
			usbi_debug(NULL, 1, "PID Check Failure\n");
			break;
		case USB_LC_STAT_UNEXP_PID:
			usbi_debug(NULL, 1, "Unexpected PID\n");
			break;
		case USB_LC_STAT_DATA_OVERRUN:
			usbi_debug(NULL, 1, "Data Exceeded Size\n");
			break;
		case USB_LC_STAT_DATA_UNDERRUN:
			usbi_debug(NULL, 1, "Less data received\n");
			break;
		case USB_LC_STAT_BUFFER_OVERRUN:
			usbi_debug(NULL, 1, "Buffer Size Exceeded\n");
			break;
		case USB_LC_STAT_BUFFER_UNDERRUN:
			usbi_debug(NULL, 1, "Buffer Underrun\n");
			break;
		case USB_LC_STAT_TIMEOUT:
			usbi_debug(NULL, 1, "Command Timed Out\n");
			break;
		case USB_LC_STAT_NOT_ACCESSED:
			usbi_debug(NULL, 1, "Not Accessed by h/w\n");
			break;
		case USB_LC_STAT_UNSPECIFIED_ERR:
			usbi_debug(NULL, 1, "Unspecified Error\n");
			break;
		case USB_LC_STAT_NO_BANDWIDTH:
			usbi_debug(NULL, 1, "No Bandwidth\n");
			break;
		case USB_LC_STAT_HW_ERR:
			usbi_debug(NULL, 1, "Host Controller h/w Error\n");
			break;
		case USB_LC_STAT_SUSPENDED:
			usbi_debug(NULL, 1, "Device was Suspended\n");
			break;
		case USB_LC_STAT_DISCONNECTED:
			usbi_debug(NULL, 1, "Device was Disconnected\n");
			break;
		case USB_LC_STAT_INTR_BUF_FULL:
			usbi_debug(NULL, 1, "Interrupt buffer was full\n");
			break;
		case USB_LC_STAT_INVALID_REQ:
			usbi_debug(NULL, 1, "Request was Invalid\n");
			break;
		case USB_LC_STAT_INTERRUPTED:
			usbi_debug(NULL, 1, "Request was Interrupted\n");
			break;
		case USB_LC_STAT_NO_RESOURCES:
			usbi_debug(NULL, 1, "No resources available for "
			    "request\n");
			break;
		case USB_LC_STAT_INTR_POLLING_FAILED:
			usbi_debug(NULL, 1, "Failed to Restart Poll");
			break;
		default:
			usbi_debug(NULL, 1, "Error Not Determined %d\n",
			    status);
			break;
		}
	} else {
		usbi_debug(NULL, 1, "read stat error: %s",strerror(errno));
		status = -1;
	}

	return (status);
}

/* return the number of bytes read/written */
static int
usb_do_io(int fd, int stat_fd, char *data, size_t size, int flag, int *status)
{
	int error;
	int ret = -1;

	usbi_debug(NULL, 4,
		"usb_do_io(): TID=%x fd=%d statfd=%d size=0x%x flag=%s\n",
		pthread_self(), fd, stat_fd, size, flag?"WRITE":"READ");

	if (size == 0) {
		return (0);
	}

	switch (flag) {
	case READ:
		ret = read(fd, data, size);
		usbi_debug(NULL, 4, "TID=%x io READ errno=%d(%s) ret=%d",
			pthread_self(), errno,strerror(errno), ret);
		usb_dump_data(data, size);
		break;
	case WRITE:
		usb_dump_data(data, size);
		ret = write(fd, data, size);
		usbi_debug(NULL, 4, "TID=%x io WRITE errno=%d(%s) ret=%d",
			pthread_self(), errno,strerror(errno),ret);
		break;
	}
	if (ret < 0) {
		int save_errno = errno;

		/* sunos_usb_get_status will do a read and overwrite errno */
		error = sunos_usb_get_status(stat_fd);
		usbi_debug(NULL, 1, "io status=%d errno=%d(%s)",error,
			save_errno,strerror(save_errno));

		if (status) {
			*status = ugen_lc2libstat(error);
		}

		/* The errno is not dependable in a muli-thread application */
		return (*status);

	} else if (status) {
		*status = 0;
	}

	usbi_debug(NULL, 4, "usb_do_io(): TID=%x amount=%d\n", pthread_self(),
		ret);

	return (ret);
}



static int
usb_check_device_and_status_open(struct usbi_dev_handle *hdev,uint8_t ifc,
    uint8_t ep_addr, int ep_type)
{
	uint8_t ep_index;
	char filename[PATH_MAX + 1], statfilename[PATH_MAX + 1];
	char cfg_num[16], alt_num[16];
	int fd, fdstat, mode;

	ep_index = usb_ep_index(ep_addr);
	
	if ((hdev->config_value == -1) || 
		(hdev->claimed_ifs[ifc].clm == -1) ||
		(hdev->claimed_ifs[ifc].altsetting == -1)) {

		usbi_debug(hdev->lib_hdl, 1, "interface not claimed");

		return (EACCES);
	}

	usbi_debug(hdev->lib_hdl, 4, "Being: TID=%x hdev=%p ifc=%x ep=%x(%x)"
		" eptype=%x,dfd=%d sfd=%d", pthread_self(), hdev, ifc,
		ep_addr, ep_index, ep_type,
		(int)hdev->priv->eps[ep_index].datafd,
		(int)hdev->priv->eps[ep_index].statfd);


	if (ifc != hdev->priv->ep_interface[ep_index]) {
		usbi_debug(hdev->lib_hdl, 1,
			"ep %d not belong to the claimed interface",
			ep_addr);

		return (EACCES);
	}

	/* ep already opened */
	if ((hdev->priv->eps[ep_index].datafd > 0) &&
	    (hdev->priv->eps[ep_index].statfd > 0)) {

		usbi_debug(hdev->lib_hdl, 4,
			"ep %d already opened,return success",
			ep_addr);

		return (0);
	}
	
	/* create filename */
	if (hdev->priv->config_index > 0) {
		(void) snprintf(cfg_num, sizeof (cfg_num), "cfg%d",
		    hdev->config_value);
	} else {
		(void) memset(cfg_num, 0, sizeof (cfg_num));
	}

	if (hdev->claimed_ifs[ifc].altsetting > 0) {
		(void) snprintf(alt_num, sizeof (alt_num), ".%d",
		    hdev->claimed_ifs[ifc].altsetting);
	} else {
		(void) memset(alt_num, 0, sizeof (alt_num));
	}

	(void) snprintf(filename, PATH_MAX, "%s/%sif%d%s%s%d",
	    hdev->idev->priv->ugenpath, cfg_num, ifc,
	    alt_num, (ep_addr & USB_ENDPOINT_DIR_MASK) ? "in" :
	    "out", (ep_addr & USB_ENDPOINT_ADDRESS_MASK));

	usbi_debug(hdev->lib_hdl, 4, "TID=%d ep %d(%x) node name: %s",
		pthread_self(), ep_addr, ep_addr, filename);

	(void) snprintf(statfilename, PATH_MAX, "%sstat", filename);


	/*
	 * for interrupt IN endpoints, we need to enable one xfer
	 * mode before opening the endpoint
	 */
	if ((ep_type == USB_ENDPOINT_TYPE_INTERRUPT) &&
	    (ep_addr & USB_ENDPOINT_IN)) {
		char control = USB_EP_INTR_ONE_XFER;
		int count;

		/* open the status device node for the ep first RDWR */
		if ((fdstat = open(statfilename, O_RDWR)) == -1) {
			usbi_debug(hdev->lib_hdl, 1,
				"can't open %s RDWR: %d",
				statfilename, errno);
		} else {
			count = write(fdstat, &control, sizeof (control));

			if (count != 1) {
				/* this should have worked */
				usbi_debug(hdev->lib_hdl, 1,
					"can't write to %s: %d",
					statfilename, errno);
				(void) close(fdstat);


				return (errno);
			}
			/* close status node and open xfer node first */
			close (fdstat);
		}
	}

	/* open the xfer node first in case alt needs to be changed */
	if (ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
		usbi_debug(hdev->lib_hdl, 4, "Open ISOC endpoint");
		mode = O_RDWR;
	} else if (ep_addr & USB_ENDPOINT_IN) {
		mode = O_RDONLY;
	} else {
		mode = O_WRONLY;
	}

	/* IMPORTANT: must open data xfer node first and then open stat node
	 * Otherwise, it will fail on multi-config or multi-altsetting devices
	 * with "Device Busy" error. See ugen_epxs_switch_cfg_alt() and 
	 * ugen_epxs_check_alt_switch() in ugen driver source code.
	 */
	if ((fd = open(filename, mode)) == -1) {
		usbi_debug(hdev->lib_hdl, 1, "can't open %s: %d(%s) TID=%x",
			filename, errno, strerror(errno),pthread_self());

		return (errno);
	}

	/* open the status node */
	if ((fdstat = open(statfilename, O_RDONLY)) == -1) {
		usbi_debug(hdev->lib_hdl, 1, "can't open %s: %d", statfilename,
			errno);

		(void) close(fd);

		return (errno);
	}

	hdev->priv->eps[ep_index].datafd = fd;
	hdev->priv->eps[ep_index].statfd = fdstat;
	usbi_debug(hdev->lib_hdl, 4, "datafd=%d, statfd=%d", fd, fdstat);

	return (0);	
}

/*
 * submit control request on default endpoint 0
 */
static int
solaris_submit_ctrl_on_default(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret=-1;
	unsigned char data[USBI_CONTROL_SETUP_LEN];
	openusb_ctrl_request_t *ctrl;

	ctrl = io->req->req.ctrl;
	data[0] = ctrl->setup.bmRequestType;
	data[1] = ctrl->setup.bRequest;
	data[2] = ctrl->setup.wValue & 0xFF;
	data[3] = (ctrl->setup.wValue >> 8) & 0xFF;
	data[4] = ctrl->setup.wIndex & 0xFF;
	data[5] = (ctrl->setup.wIndex >> 8) & 0xFF;
	data[6] = ctrl->length & 0xFF;
	data[7] = (ctrl->length >> 8) & 0xFF;

	usbi_debug(hdev->lib_hdl, 4, "ep0:data%d ,stat%d",
		hdev->priv->eps[0].datafd,
		hdev->priv->eps[0].statfd);

	if (hdev->priv->eps[0].datafd == -1) {
		usbi_debug(hdev->lib_hdl, 1, "ep0 not opened");

		return (OPENUSB_NOACCESS);
	}

	if ((data[0] & USB_REQ_DIR_MASK) == USB_REQ_DEV_TO_HOST) {
		ret = usb_do_io(hdev->priv->eps[0].datafd,
		    hdev->priv->eps[0].statfd, (char *)data,
		    USBI_CONTROL_SETUP_LEN, WRITE,
		    &ctrl->result.status);
	} else {
		char *buf;

		if ((buf = malloc(ctrl->length+ USBI_CONTROL_SETUP_LEN)) ==
		    NULL) {
			usbi_debug(hdev->lib_hdl, 1,
				"alloc for ctrl out failed");

			return (OPENUSB_NO_RESOURCES);
		}
		(void) memcpy(buf, data, USBI_CONTROL_SETUP_LEN);
		(void) memcpy(buf + USBI_CONTROL_SETUP_LEN, ctrl->payload,
		    ctrl->length);

		ret = usb_do_io(hdev->priv->eps[0].datafd,
			hdev->priv->eps[0].statfd, buf,
			ctrl->length + USBI_CONTROL_SETUP_LEN, WRITE,
			&ctrl->result.status);

		free(buf);
	}

	if (ret < USBI_CONTROL_SETUP_LEN) {
		usbi_debug(hdev->lib_hdl, 1, "error sending control msg: %d",
			ret);
		
		ctrl->result.status = ret;
		ctrl->result.transferred_bytes = 0;
		io->status = USBI_IO_COMPLETED;
		return (OPENUSB_PLATFORM_FAILURE);
	}

	ret -= USBI_CONTROL_SETUP_LEN;

	/* Read the remaining bytes for IN request */
	if ((ctrl->length) && ((data[0] & USB_REQ_DIR_MASK) ==
	    USB_REQ_DEV_TO_HOST)) {
		ret = usb_do_io(hdev->priv->eps[0].datafd,
			hdev->priv->eps[0].statfd, (char *)ctrl->payload,
			ctrl->length, READ,
			&ctrl->result.status);
	}

	usbi_debug(NULL, 4, "send ctrl bytes %d", ret);
	io->status = USBI_IO_COMPLETED;
	if (ret >= 0) {
		ctrl->result.transferred_bytes = ret;
	}

	return (ret);
}

/*
 * submit control request on non-zero control pipe
 * FIXME: don't have a device to test this function
 */
static int
solaris_submit_ctrl_non_default(struct usbi_dev_handle *hdev,
	struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;
	openusb_ctrl_request_t *ctrl;

	ctrl = io->req->req.ctrl;

	ep_addr = io->req->endpoint;
	ep_index = usb_ep_index(ep_addr);

	pthread_mutex_lock(&hdev->lock);

	if ((ret = usb_check_device_and_status_open(hdev, io->req->interface,
	    ep_addr, USB_ENDPOINT_TYPE_BULK)) != 0) {
		usbi_debug(hdev->lib_hdl, 1,
			"check_device_and_status_open for ep %d failed",
			ep_addr);

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_NOACCESS);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)ctrl->payload,
		    ctrl->length, READ, &ctrl->result.status);
	} else {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)ctrl->payload,
		    ctrl->length, WRITE, &ctrl->result.status);
		    
	}
	if (ret >= 0) {
		ctrl->result.transferred_bytes = ret;
	}

	pthread_mutex_unlock(&hdev->lock);

	usbi_debug(hdev->lib_hdl, 4, "send ctrl bytes %d", ret);
	io->status = USBI_IO_COMPLETED;

	return (ret);
}

static int
solaris_submit_ctrl(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	if (io->req->endpoint == 0) {
		return (solaris_submit_ctrl_on_default(hdev, io));
	} else {
		return (solaris_submit_ctrl_non_default(hdev, io));
	}
}

static int
solaris_submit_bulk(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;
	openusb_bulk_request_t *bulk;

	bulk = io->req->req.bulk;

	ep_addr = io->req->endpoint;
	ep_index = usb_ep_index(ep_addr);

	pthread_mutex_lock(&hdev->lock);

	if ((ret = usb_check_device_and_status_open(hdev,io->req->interface,
	    ep_addr, USB_ENDPOINT_TYPE_BULK)) != 0) {
		usbi_debug(hdev->lib_hdl, 1,
			"check_device_and_status_open for ep %d failed",
			ep_addr);

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_NOACCESS);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)bulk->payload,
		    bulk->length, READ, &bulk->result.status);
	} else {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)bulk->payload,
		    bulk->length, WRITE, &bulk->result.status);
		    
	}
	if (ret >= 0) {
		bulk->result.transferred_bytes = ret;
	}

	pthread_mutex_unlock(&hdev->lock);

	usbi_debug(hdev->lib_hdl, 4, "send bulk bytes %d", ret);
	io->status = USBI_IO_COMPLETED;

	return (ret);
}

static int
solaris_submit_intr(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;
	openusb_intr_request_t *intr;
	
	intr = io->req->req.intr;

	ep_addr = io->req->endpoint;
	ep_index = usb_ep_index(ep_addr);

	pthread_mutex_lock(&hdev->lock);

	usbi_debug(hdev->lib_hdl, 4, "Begin: TID=%d",pthread_self());

	if ((ret = usb_check_device_and_status_open(hdev,io->req->interface,
	    ep_addr, USB_ENDPOINT_TYPE_INTERRUPT)) != 0) {
		usbi_debug(hdev->lib_hdl, 1,
			"check_device_and_status_open for ep %d failed",
			ep_addr);

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_NOACCESS);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)intr->payload,
		    intr->length, READ, &intr->result.status);

		/* close the endpoint so we stop polling the endpoint now */
		(void) close(hdev->priv->eps[ep_index].datafd);
		(void) close(hdev->priv->eps[ep_index].statfd);
		hdev->priv->eps[ep_index].datafd = -1;
		hdev->priv->eps[ep_index].statfd = -1;
	} else {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		    hdev->priv->eps[ep_index].statfd, (char *)intr->payload,
		    intr->length, WRITE, &intr->result.status);
	}

	usbi_debug(hdev->lib_hdl, 4, "send intr bytes %d", ret);

	if (ret >= 0) {
		intr->result.transferred_bytes = ret;
	}

	usbi_debug(hdev->lib_hdl, 4,"Intr status= %d\n",intr->result.status);

	io->status = USBI_IO_COMPLETED;
	pthread_mutex_unlock(&hdev->lock);

	return (ret);
}


/*
 * isoc data = isoc pkt header + isoc pkt desc + isoc pkt desc ...+ data
 */
static int
solaris_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;
	struct openusb_isoc_packet *packet;
	struct usbk_isoc_pkt_header *header;
	struct usbk_isoc_pkt_descr *pkt_descr;
	ushort_t n_pkt, pkt;
	uint_t pkts_len = 0, len;
	char *p, *buf;
	openusb_isoc_request_t *isoc;

	usbi_debug(hdev->lib_hdl, 4, "Begin: TID=%x",pthread_self());

	isoc = io->req->req.isoc;
	if (isoc->flags == 1) {
	/* what's this flag for? */
		usbi_debug(hdev->lib_hdl, 1, "wrong isoc request flags");

		return (OPENUSB_BADARG);
	}

	ep_addr = io->req->endpoint;
	ep_index = usb_ep_index(ep_addr);

	/* have to globally lock this function to prevent multi thread enter
	 * the same code and access the same device. Maybe every pipe should 
	 * have a lock.
	 */
	pthread_mutex_lock(&hdev->lock);

	if ((ret = usb_check_device_and_status_open(hdev,io->req->interface,
	    ep_addr, USB_ENDPOINT_TYPE_ISOCHRONOUS)) != 0) {
		usbi_debug(hdev->lib_hdl, 1,
			"check_device_and_status_open for ep %d failed",
			ep_addr);

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_NOACCESS);
	}

	n_pkt = isoc->pkts.num_packets;
	packet = isoc->pkts.packets;
	for (pkt = 0; pkt < n_pkt; pkt++) {
		pkts_len += packet[pkt].length;
		/* sum of all packets payload length */
	}

	if (pkts_len == 0) {
		usbi_debug(hdev->lib_hdl, 1, "pkt length invalid");

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_BADARG);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
	/* IN pipe, only header length + desc length */
		len = sizeof (int) +
		    sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	} else {
	/* OUT pipe, len = header length + desc length + data length */
		len = pkts_len + sizeof (int)
		    + sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	}

	if ((buf = (char *)malloc(len)) == NULL) {
		usbi_debug(hdev->lib_hdl, 1,
			"malloc isoc out buf of length %d failed",
			len);

		pthread_mutex_unlock(&hdev->lock);
		return (OPENUSB_NO_RESOURCES);
	}

	usbi_debug(hdev->lib_hdl, 4, "endpoint:%02x, len=%d", ep_addr, len);
	memset(buf, 0, len);

	/* isoc packet header */
	header = (struct usbk_isoc_pkt_header *)buf;
	header->isoc_pkts_count = n_pkt;

	/* isoc packet descs */
	p = buf + sizeof (int);
	pkt_descr = (struct usbk_isoc_pkt_descr *)p;

	/* data */
	p += sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	for (pkt = 0; pkt < n_pkt; pkt++) {
		/* prepare packet desc info */
		pkt_descr[pkt].isoc_pkt_length = packet[pkt].length;
		pkt_descr[pkt].isoc_pkt_actual_length = 0;
		pkt_descr[pkt].isoc_pkt_status = 0;

		if ((ep_addr & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT) {
		/* prepare data */
			memcpy((void *)p, packet[pkt].payload,
				packet[pkt].length);
			p += packet[pkt].length;
		}
	}
	
	usbi_debug(hdev->lib_hdl, 4, "total header len=%d, payload len=%d",
		len, pkts_len);

	/* we should use lock to prevent multi threads access the same device
	 * simutaneously, or some thread close the device while another is 
	 * accessing it? 
	 */

	/* do isoc OUT xfer or init polling for isoc IN xfer */
	ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
		hdev->priv->eps[ep_index].statfd, (char *)buf, len, WRITE,
		&isoc->isoc_status);

	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1, "write isoc ep failed %d TID=%d",
			ret, pthread_self());

		pthread_mutex_unlock(&hdev->lock);
		free(buf);

		return (OPENUSB_PLATFORM_FAILURE);
	}

	/* Read an OUT request's packets status */
	if ((ep_addr & USB_ENDPOINT_DIR_MASK) == 0) {
		ret = usb_do_io(hdev->priv->eps[ep_index].datafd,
			hdev->priv->eps[ep_index].statfd, (char *)buf,
			sizeof (struct usbk_isoc_pkt_descr) * n_pkt,
			READ, &isoc->isoc_status);

		if (ret < 0) {
			usbi_debug(hdev->lib_hdl, 1,
				"get status failed %d TID=%d",
				ret, pthread_self());

			pthread_mutex_unlock(&hdev->lock);

			free(buf);

			return (OPENUSB_PLATFORM_FAILURE);

		}

		pkt_descr = (struct usbk_isoc_pkt_descr *)buf;

		packet = isoc->pkts.packets;

		for(pkt = 0; pkt < n_pkt; pkt++) {
			usbi_debug(hdev->lib_hdl, 4, "packet: %d, len: %d",
				pkt, packet[pkt].length);
			isoc->isoc_results[pkt].status = 
				pkt_descr[pkt].isoc_pkt_status;
			isoc->isoc_results[pkt].transferred_bytes = 
				pkt_descr[pkt].isoc_pkt_actual_length;
		}
	}

	free(buf);

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {

		len = pkts_len + n_pkt*sizeof(struct usbk_isoc_pkt_descr);

		usbi_debug(hdev->lib_hdl, 4, "Total length = %d, pkts_len=%d\n",
			len, pkts_len);

		buf = malloc(len);
		if(!buf) {
			pthread_mutex_unlock(&hdev->lock);
			return OPENUSB_NO_RESOURCES;
		}
		memset(buf, 0, len);

		/* packet descr at beginning */
		pkt_descr = (struct usbk_isoc_pkt_descr *)buf;

		ret = usb_do_io(hdev->priv->eps[ep_index].datafd, 
			hdev->priv->eps[ep_index].statfd,
			(char *)buf, len, READ, &isoc->isoc_status);


		if (ret < 0) {
			usbi_debug(hdev->lib_hdl, 1, "read isoc ep failed %d",
				ret);

			free(buf);

			pthread_mutex_unlock(&hdev->lock);
			return (OPENUSB_PLATFORM_FAILURE);
		}
		
		usbi_debug(hdev->lib_hdl, 4, "Read isoc data: n_pkt=%d",n_pkt);

		packet = isoc->pkts.packets;
		p = buf;
		p += n_pkt * sizeof(struct usbk_isoc_pkt_descr);

		for(pkt = 0; pkt < n_pkt; pkt++) {
			usbi_debug(hdev->lib_hdl, 4, "packet: %d, len: %d", pkt,
					packet[pkt].length);
			memcpy(packet[pkt].payload, p, packet[pkt].length); 
			p += packet[pkt].length;
			isoc->isoc_results[pkt].status = 
				pkt_descr[pkt].isoc_pkt_status;
			isoc->isoc_results[pkt].transferred_bytes = 
				pkt_descr[pkt].isoc_pkt_actual_length;
		}

		free(buf);

		/* we have to close this pipe to stop ISOC IN polling */
		(void) close(hdev->priv->eps[ep_index].datafd);
		(void) close(hdev->priv->eps[ep_index].statfd);

		hdev->priv->eps[ep_index].datafd = -1;
		hdev->priv->eps[ep_index].statfd = -1;
	}

	pthread_mutex_unlock(&hdev->lock);
	io->status = USBI_IO_COMPLETED;
	return (0);
}

/*
 * arguments validity already checked at frontend
 * We may safely assume all arguments are valid here
 */
static int
solaris_set_altinterface(struct usbi_dev_handle *hdev, uint8_t ifc, uint8_t alt)
{
	struct usbi_device *idev = hdev->idev;
	int index, iface;

	if (idev->priv->info.claimed_interfaces[ifc] != hdev) {
		usbi_debug(hdev->lib_hdl, 1, "handle dismatch");

		return (OPENUSB_PLATFORM_FAILURE);
	}

	usb_close_all_eps(hdev);
	iface = ifc;
	index = hdev->config_value - 1;

	/* set alt interface is implicitly done when endpoint is opened */
	hdev->claimed_ifs[ifc].altsetting = alt;

	usb_set_ep_iface_alts(hdev, index, iface, alt);

	return (OPENUSB_SUCCESS);
}

static int solaris_get_altinterface(struct usbi_dev_handle *hdev,uint8_t ifc,
	uint8_t *alt)
{
	if (!hdev) {
		return OPENUSB_BADARG;
	}
	
	if(ifc > USBI_MAXINTERFACES) {
		return OPENUSB_BADARG;
	}
	*alt = hdev->claimed_ifs[ifc].altsetting;
	
	return OPENUSB_SUCCESS;
}

static int
solaris_io_cancel(struct usbi_io *io)
{
	struct usbi_dev_handle *hdev = io->dev;
	
	usbi_debug(NULL, 4, "cancel io %p",io);
	if(io->status == USBI_IO_INPROGRESS) {
		list_del(&io->list);
		io->status = USBI_IO_CANCEL;
		
		pthread_mutex_lock(&hdev->lib_hdl->complete_lock);

		list_add(&io->list,&hdev->lib_hdl->complete_list);
		pthread_cond_signal(&hdev->lib_hdl->complete_cv);
		hdev->lib_hdl->complete_count++;

		pthread_mutex_unlock(&hdev->lib_hdl->complete_lock);
	}

	return (OPENUSB_SUCCESS);
}

static int
solaris_init(struct usbi_handle *hdl, uint32_t flags )
{
	int ret;
	
	usbi_debug(NULL, 4, "Begin");

	if(solaris_back_inited != 0) {
	/*already inited */
		usbi_debug(NULL, 1, "Already inited");
		solaris_back_inited++; /* openusb_init gets called once more */
		return 0;
	}
	
	ret = pthread_mutex_init(&cb_io_lock, NULL);
	if (ret < 0) {
		usbi_debug(NULL, 1, "initing mutex failed(ret = %d)", ret);

		return (OPENUSB_PLATFORM_FAILURE);
	}

	ret = pthread_cond_init(&cb_io_cond, NULL);
	if (ret < 0) {
		usbi_debug(NULL, 1, "initing cond failed(ret = %d)", ret);
		pthread_mutex_destroy(&cb_io_lock);

		return (OPENUSB_PLATFORM_FAILURE);
	}


#ifdef OPENUSB_USE_HAL /* hal */
	if (!g_thread_supported ())
		g_thread_init (NULL);

	ret = pthread_create(&hotplug_thread, NULL,
		(void *) hal_hotplug_event_thread, NULL);
	if (ret < 0) {
		usbi_debug(NULL, 1, "unable to create polling callback thread"
		    "(ret = %d)", ret);
		pthread_cond_destroy(&cb_io_cond);
		pthread_mutex_destroy(&cb_io_lock);

		return (OPENUSB_PLATFORM_FAILURE);

	}
#endif

	solaris_back_inited++;

	usbi_debug(NULL, 4, "End");

	return (OPENUSB_SUCCESS);
}

void solaris_fini(struct usbi_handle *hdl)
{
	if(solaris_back_inited == 0) { /*already fini */
		return;
	}
	/* pthread_cancel(cb_thread); *//*stop this thread */
	pthread_mutex_destroy(&cb_io_lock);
	pthread_cond_destroy(&cb_io_cond);

	if (solaris_back_inited == 1) {
		usbi_debug(NULL, 4, "stop hotplug thread");
		g_main_loop_quit(event_loop); /* quit loop, free resources */
		pthread_cancel(hotplug_thread); /* stop the hotplug thread */
		pthread_join(hotplug_thread, NULL);
	}
	solaris_back_inited--;
	return;
}

struct usbi_backend_ops backend_ops = {
	.backend_version		= 1,
	.io_pattern			= PATTERN_SYNC,
	.init				= solaris_init,
	.fini				= solaris_fini,
	.find_buses			= solaris_find_busses,
	.refresh_devices		= solaris_refresh_devices,
	.free_device			= solaris_free_device,
	.dev = {
		.open				= solaris_open,
		.close				= solaris_close,
		.set_configuration		= solaris_set_configuration,
		.get_configuration		= solaris_get_configuration,
		.claim_interface		= solaris_claim_interface,
		.release_interface		= solaris_release_interface,
		.get_altsetting			= solaris_get_altinterface,
		.set_altsetting			= solaris_set_altinterface,
		.reset				= NULL,
		.get_driver_np		= NULL,
		.attach_kernel_driver_np	= NULL,
		.detach_kernel_driver_np	= NULL,
		.ctrl_xfer_wait			= solaris_submit_ctrl,
		.intr_xfer_wait			= solaris_submit_intr,
		.bulk_xfer_wait			= solaris_submit_bulk,
		.isoc_xfer_wait			= solaris_submit_isoc,
		.io_cancel			= solaris_io_cancel,
		.get_raw_desc			= solaris_get_raw_desc,
	},
};

