/*
 * Handling of busses and devices
 *
 * Copyright (c) 2007 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <errno.h>
#include <stdlib.h>	/* malloc, free */
#include <string.h>	/* memcpy */

#include <pthread.h>	/*pthread */

#include "usbi.h"

extern struct list_head backends;
extern struct usbi_list usbi_buses;
extern struct usbi_list usbi_devices;

static libusb_busid_t cur_bus_id = 1;
static libusb_devid_t cur_device_id = 1;

/*
 * Bus code
 */
void usbi_add_bus(struct usbi_bus *ibus, struct usbi_backend *backend)
{
	/* FIXME: Handle busid rollover gracefully? */
	ibus->busid = cur_bus_id++;

	ibus->ops = backend->ops;

	list_init(&(ibus->devices.head));

	list_add(&ibus->list, &usbi_buses.head);

	/* backend initialize the following elements. Should be moved to
	 * frontend.
	 *  ibus->lock
	 *  ibus->devices list
	 */
}

void usbi_free_bus(struct usbi_bus *ibus)
{
	/*FIXME: what about the ibus->devices list */
	if (ibus->priv) {
		free(ibus->priv);
	}
	free(ibus);
}

void usbi_remove_bus(struct usbi_bus *ibus)
{
	list_del(&ibus->list);
	usbi_free_bus(ibus);
}

struct usbi_bus *usbi_find_bus_by_id(libusb_busid_t busid)
{
	struct usbi_bus *ibus;

	/* FIXME: We should probably index the device id in
	 * a rbtree or something
	 */
	list_for_each_entry(ibus, &usbi_buses.head, list) {
	/* safe */
		if (ibus->busid == busid)
			return ibus;
	}

	return NULL;
}

static void refresh_bus(struct usbi_backend *backend)
{
	struct list_head busses;
	struct usbi_bus *ibus, *tibus;
	int ret;

	list_init(&busses);
	
	if (!backend->ops->find_buses) {
		usbi_debug(NULL, 1, "NULL bus");
		return;
	}

	ret = backend->ops->find_buses(&busses);
	if (ret < 0)
		return;

	/*
	 * Now walk through all of the busses we know about and compare against
	 * this new list. Any duplicates will be removed from the new list.
	 * If we don't find it in the new list, the bus was removed. Any
	 * busses still in the new list, are new to us.
	 */
	list_for_each_entry_safe(ibus, tibus, &usbi_buses.head, list) {
		struct usbi_bus *nibus, *tnibus;
		int found = 0;

		list_for_each_entry_safe(nibus, tnibus, &busses, list) {
			if (ibus->busnum == nibus->busnum) {
				/* Remove it from the new devices list */
				list_del(&nibus->list);

				usbi_free_bus(nibus);
				found = 1;
				break;
			}
		}

		if (!found)
			/* The bus was removed from the system */
			list_del(&ibus->list);
	}

	/*
	 * Anything on the *busses list is new. So add them to usbi_buses
	 * and process them like the new bus they are
	 */
	list_for_each_entry_safe(ibus, tibus, &busses, list) {
		list_del(&ibus->list);
		usbi_add_bus(ibus, backend);
	}
}

static void usbi_refresh_busses(void)
{
	struct usbi_backend *backend, *tbackend;

	list_for_each_entry_safe(backend, tbackend, &backends, list) {
		refresh_bus(backend);
	}
}

/*
 * Device code
 */
void usbi_add_device(struct usbi_bus *ibus, struct usbi_device *idev)
{
	struct usbi_handle *handle, *thdl;

	/* FIXME: Handle devid rollover gracefully? */
	idev->devid = cur_device_id++;

	idev->bus = ibus;
	idev->ops = &ibus->ops->dev;
	
	/*FIXME: lock here */
	list_add(&idev->bus_list, &ibus->devices.head);
	list_add(&idev->dev_list, &usbi_devices.head);

	pthread_mutex_lock(&usbi_handles.lock);
	list_for_each_entry_safe(handle, thdl, &usbi_handles.head, list){
		/* every libusb instance should get notification
		 * of this event
		 */
		usbi_add_event_callback(handle, idev->devid, USB_ATTACH);
	}
	pthread_mutex_unlock(&usbi_handles.lock);
}

void usbi_free_device(struct usbi_device *idev)
{
	if (idev->children)
		free(idev->children);
	usbi_destroy_configuration(idev);
	if (idev->bus->ops->free_device)
		idev->bus->ops->free_device(idev);

	/*idev->priv is freed by backend */	

	free(idev);
}

void usbi_remove_device(struct usbi_device *idev)
{
	struct usbi_handle *handle, *thdl;

	libusb_devid_t devid = idev->devid;

	list_del(&idev->bus_list);
	list_del(&idev->dev_list);

	usbi_free_device(idev);

	pthread_mutex_lock(&usbi_handles.lock);
	list_for_each_entry_safe(handle, thdl, &usbi_handles.head, list){
		/*every libusb instance should get notification of this event */
		usbi_add_event_callback(handle,devid, USB_REMOVE);
	}
	pthread_mutex_unlock(&usbi_handles.lock);
}

/*
 * add all USB buses to system bus list
 * add all devices to its parent buses
 */
void usbi_rescan_devices(void)
{
  struct usbi_bus *ibus, *tbus;

  usbi_refresh_busses();

  list_for_each_entry_safe(ibus, tbus, &usbi_buses.head, list) {
    ibus->ops->refresh_devices(ibus);

#if 0 
      /*
       * Some platforms fetch the descriptors on scanning (like Linux) so we
       * don't need to fetch them again
       */
      if (!idev->desc.device_raw.data) {
        libusb_dev_handle_t udev;

        ret = libusb_open(idev->devid, &udev);
        if (ret >= 0) {
          usbi_fetch_and_parse_descriptors(udev);

          libusb_close(udev);
        }
      }

      /* FIXME: Handle checking the device and config descriptor seperately */
#endif
  }
}

#if 1 /* this internal interfaces defined, but not used */
int usbi_get_child_count(libusb_devid_t devid, unsigned char *count)
{
	struct usbi_device *idev;

	idev = usbi_find_device_by_id(devid);
	if (!idev)
		return LIBUSB_UNKNOWN_DEVICE;

	*count = idev->nports;

	return LIBUSB_SUCCESS;
}

int usbi_get_child_device_id(libusb_devid_t hub_devid, int port,
	libusb_devid_t *child_devid)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(hub_devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  port--;	/* 1-indexed */
  if (port < 0 || port > idev->nports)
    return LIBUSB_BADARG;

  if (!idev->children[port])
    return LIBUSB_BADARG;

  *child_devid = idev->children[port]->devid;

  return LIBUSB_SUCCESS;
}

int usbi_get_parent_device_id(libusb_devid_t child_devid,
	libusb_devid_t *hub_devid)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(child_devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  if (!idev->parent)
    return LIBUSB_BADARG;

  *hub_devid = idev->parent->devid;

  return LIBUSB_SUCCESS;
}

int usbi_get_bus_id(libusb_devid_t devid, libusb_busid_t *busid)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  *busid = idev->bus->busid;

  return LIBUSB_SUCCESS;
}

int usbi_get_devnum(libusb_devid_t devid, unsigned char *devnum)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  *devnum = idev->devnum;

  return LIBUSB_SUCCESS;
}
#endif

int32_t libusb_get_busid_list(libusb_handle_t handle, libusb_busid_t **busids,
	uint32_t *num_busids)
{
	struct usbi_handle *hdl;
	struct usbi_bus *ibus;
	libusb_busid_t *tmp;
	
	if (!busids || *busids || !num_busids) {
		return LIBUSB_BADARG;
	}

	*num_busids = 0;
	*busids = NULL;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	pthread_mutex_lock(&usbi_buses.lock);

	list_for_each_entry(ibus, &usbi_buses.head, list) {
	/* safe */
		(*num_busids)++;
	}

	if (*num_busids == 0) {
		pthread_mutex_unlock(&usbi_buses.lock);
		usbi_debug(hdl, 2, "Null list");
		return LIBUSB_NULL_LIST;
	}

	*busids = malloc((*num_busids) * sizeof (libusb_busid_t));
	if (*busids == NULL) {
		pthread_mutex_unlock(&usbi_buses.lock);
		usbi_debug(hdl, 2, "No resource");
		return LIBUSB_NO_RESOURCES;
	}

	tmp = *busids;
	list_for_each_entry(ibus, &usbi_buses.head, list) {
	/* safe */
		*tmp = ibus->busid;
		tmp++;
	}
	pthread_mutex_unlock(&usbi_buses.lock);

	return LIBUSB_SUCCESS;
}

void libusb_free_busid_list(libusb_busid_t *busids)
{
	if (busids == NULL)
		return;

	free(busids);
}

/* get the alternate setting count of an interface */
static int usbi_get_num_altsettings(struct usbi_device *idev,
	uint8_t *buffer, uint16_t buflen, uint8_t infidx)
{
	uint8_t num_alt = 0;
	uint8_t *p = buffer;

	if ((buffer == NULL) || (buflen == 0))
		return -1;

	while (buflen >= 3) {

		if ((p[1] == USB_DESC_TYPE_INTERFACE) && (p[2] == infidx)) {
			num_alt++;
		} else if ((p[1] == USB_DESC_TYPE_INTERFACE) &&
			(p[2] > infidx)) {
			break;
		}

		/* make sure bLength is not a bad value */
		if ((p[0] > 0) && (buflen >= p[0])) {
			
			/* reverse sequence of the fllowing two statements
			 * causes matching failure
			 */
			buflen -= p[0];
			p += p[0];

		} else {
			return -1;
		}
	}

	if (num_alt > 0)
		return num_alt;
	else
		return -1;
			
}

/*
 * check interface descriptor for class information,
 *
 * check device class/subclass/protocol first, then
 * interface's devclass/subclass/protocol = -1, wild
 * match
 *
 * return 1 if success 
 */
static int usbi_match_class(libusb_handle_t handle, struct usbi_device *idev,
	int16_t devclass, int16_t subclass, int16_t protocol)
{
	uint8_t *buf;
	uint16_t buflen;
	usb_device_desc_t dev_desc;
	usb_config_desc_t cfg_desc;
	usb_interface_desc_t ifc_desc;
	int ret;
	int c, i, a;
	struct usbi_handle *hdl;

	hdl = usbi_find_handle(handle);
	
	usbi_debug(NULL, 4, "devid= %d class=%d, subclass=%d, proto = %d",
		(int)idev->devid, devclass, subclass, protocol);

	if ((ret = libusb_parse_device_desc(handle, idev->devid,
		NULL, 0, &dev_desc)) < 0) {
		usbi_debug(hdl, 2, "get device desc for devid %llu "
			"failed (ret = %d)", idev->devid, ret);
		return 0;
	}

	usbi_debug(hdl, 4, "vid=%x, pid=%x, class=%d, subclass=%d, proto=%d %d",
		dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass,
		dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol,
		dev_desc.bNumConfigurations);

	if (((devclass == -1) || (devclass == dev_desc.bDeviceClass)) &&
		((subclass == -1) || (subclass == dev_desc.bDeviceSubClass)) &&
		((protocol == -1) || (protocol = dev_desc.bDeviceProtocol)))
		return 1;


	for (c = 0; c < dev_desc.bNumConfigurations; c++) {

		ret = libusb_get_raw_desc(handle, idev->devid,
			USB_DESC_TYPE_CONFIG, c, 0, &buf, &buflen);
		if (ret < 0) {
			usbi_debug(hdl, 2, "get raw config desc index %d "
				"for devid %llu failed (ret = %d)", c,
				idev->devid, ret);
			continue;
		}
		
		if ((ret = libusb_parse_config_desc(handle, idev->devid, buf,
			buflen, c, &cfg_desc)) < 0) {
			usbi_debug(hdl, 2, "parse config desc index %d "
				"for devid %d failed (ret = %d)", c,
				idev->devid, ret);
			libusb_free_raw_desc(buf);
			continue;
		}

		usbi_debug(hdl, 4, "buflen = %d, int#=%d", buflen,
			cfg_desc.bNumInterfaces);

		for (i = 0; i < cfg_desc.bNumInterfaces; i++) {
			int num_alt;

			num_alt = usbi_get_num_altsettings(idev, buf,
				buflen, i);
			if (num_alt < 0) {
				usbi_debug(NULL, 1, "altsetting error");
				continue;
			}

			for (a = 0; a < num_alt; a++) {
				if ((ret = libusb_parse_interface_desc(handle,
					idev->devid, buf, buflen, c, i, a,
					&ifc_desc)) < 0) {
					usbi_debug(hdl, 2, "get ifc desc "
						"%d-%d-%d failed"
						" (ret = %d (%s))",
						c, i, a, ret,
						libusb_strerror(ret));
					continue;
				}

				if (((devclass == -1) || (devclass ==
					ifc_desc.bInterfaceClass)) &&
					((subclass == -1) || (subclass ==
					ifc_desc.bInterfaceSubClass)) &&
					((protocol == -1) || (protocol ==
					ifc_desc.bInterfaceProtocol))) {

					libusb_free_raw_desc(buf);
					return 1;
				}
			}
		}

		libusb_free_raw_desc(buf);
	}

	return 0;
}

/*
 * Get all devices on bus
 *	if busid = 0, wild match, return all devices on the system
 *	else return devices on that busid
 * NOTE: application must pass a NULL devids. Otherwise, memory will leak
 */
int32_t libusb_get_devids_by_bus(libusb_handle_t handle, libusb_busid_t busid,
	libusb_devid_t **devids, uint32_t *num_devids)
{
	struct usbi_handle *hdl;
	struct usbi_bus *ibus;
	struct usbi_device *idev;
	libusb_devid_t *tdevid;
	int32_t devcnts=0;
	
	/* if *devid is not NULL, there will be possible memleaks */
	if (!num_devids || !devids) {
		return LIBUSB_BADARG;
	}

	*num_devids = 0;
	*devids = NULL;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if (busid == 0) {
		/* get all devids */
		pthread_mutex_lock(&usbi_devices.lock);
		list_for_each_entry(idev, &usbi_devices.head, dev_list) {
		/* safe */
			devcnts++;
		}

		if ( devcnts== 0) {
			pthread_mutex_unlock(&usbi_devices.lock);
			return LIBUSB_NULL_LIST;
		}

		*devids = malloc((devcnts) * sizeof (libusb_devid_t));
		if (*devids == NULL) {
			pthread_mutex_unlock(&usbi_devices.lock);
			return LIBUSB_NO_RESOURCES;
		}

		tdevid = *devids;
		list_for_each_entry(idev, &usbi_devices.head, dev_list) {
		/* safe */
			*tdevid = idev->devid;
			tdevid++;
		}

		*num_devids = devcnts;

		pthread_mutex_unlock(&usbi_devices.lock);

		return LIBUSB_SUCCESS;
	}

	ibus = usbi_find_bus_by_id(busid);
	if (!ibus)
		return LIBUSB_UNKNOWN_DEVICE;

	pthread_mutex_lock(&ibus->devices.lock);

	if (list_empty(&ibus->devices.head)) {
		pthread_mutex_unlock(&ibus->devices.lock);
		return LIBUSB_NULL_LIST;
	}

	list_for_each_entry(idev, &ibus->devices.head, bus_list) {
	/* safe */
		devcnts++;
	}

	if (devcnts == 0) {
		pthread_mutex_unlock(&ibus->devices.lock);
		return LIBUSB_NULL_LIST;
	}

	*devids = malloc(devcnts * sizeof (libusb_devid_t));
	if (*devids == NULL) {
		pthread_mutex_unlock(&ibus->devices.lock);
		return LIBUSB_NO_RESOURCES;
	}

	tdevid = *devids;
	list_for_each_entry(idev, &ibus->devices.head, bus_list) {
	/* safe */
		*tdevid = idev->devid;
		tdevid++;
	}
	*num_devids = devcnts;
	pthread_mutex_unlock(&ibus->devices.lock);

	return LIBUSB_SUCCESS;
}

/*
 * Get device of specified vendor/product
 *	vendor = -1, wild match, match any vendor
 *	product = -1, wild match, match any product
 */
int32_t libusb_get_devids_by_vendor(libusb_handle_t handle, int32_t vendor,
	int32_t product, libusb_devid_t **devids, uint32_t *num_devids)
{
	struct usbi_handle *hdl;
	struct usbi_device *idev=NULL;
	struct usbi_device *tdev;
	libusb_devid_t *tdevid;
	struct list_head match_list;
	int ret;

	usbi_debug(NULL, 4, "Begin");

	/* if *devid is not NULL, there will be possible memleaks */
	if (!num_devids || !devids || *devids) {
		return LIBUSB_BADARG;
	}

	*num_devids = 0;
	*devids = NULL;
	list_init(&match_list);

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if ((vendor < -1) || (vendor > 0xffff) || (product < -1) ||
		(product > 0xffff))
		return LIBUSB_BADARG;

	pthread_mutex_lock(&usbi_devices.lock);
	list_for_each_entry_safe(idev, tdev, &usbi_devices.head, dev_list) {
		usb_device_desc_t desc;
		uint16_t Vendor;
		uint16_t Product;

		pthread_mutex_unlock(&usbi_devices.lock);
		if ((ret = libusb_parse_device_desc(handle, idev->devid,
				NULL, 0, &desc)) < 0) {

			usbi_debug(hdl, 2, "get device desc for devid %d "
					"failed (ret = %d)", idev->devid, ret);

			pthread_mutex_lock(&usbi_devices.lock);

			continue;
		}

		pthread_mutex_lock(&usbi_devices.lock);
		
		Vendor = libusb_le16_to_cpu(desc.idVendor);
		Product = libusb_le16_to_cpu(desc.idProduct);
		if (((vendor == -1) || (vendor == Vendor)) &&
			((product == -1) || (product == Product))) {

			list_add(&idev->match_list, &match_list);
			(*num_devids)++;
		}
	}

	if (*num_devids == 0) {
		pthread_mutex_unlock(&usbi_devices.lock);
		return LIBUSB_NULL_LIST;
	}

	*devids = malloc((*num_devids) * sizeof (libusb_devid_t));
	if (*devids == NULL) {
		pthread_mutex_unlock(&usbi_devices.lock);
		return LIBUSB_NO_RESOURCES;
	}

	tdevid = *devids;
	list_for_each_entry(idev, &match_list, match_list) {
	/* safe */
		*tdevid = idev->devid;
		tdevid++;
	}
	pthread_mutex_unlock(&usbi_devices.lock);

	return LIBUSB_SUCCESS;
}

/*
 * Get devices of specified class/subclass/protocol
 *	devclass = -1, wild match
 *	subclass = -1, wild match
 *	protocol = -1, wild match
 * NOTE: application must not pass a non-NULL devids, otherwise memory
 *	will leak
 */
int32_t libusb_get_devids_by_class(libusb_handle_t handle, int16_t devclass,
	int16_t subclass, int16_t protocol, libusb_devid_t **devids,
	uint32_t *num_devids)
{
	struct usbi_handle *hdl;
	struct usbi_device *idev, *tdev;
	libusb_devid_t *tdevid;
	struct list_head match_list;
	
	/* if *devid is not NULL, there will be possible memleaks */
	if (!num_devids || !devids) /* || *devids)*/ {
		return LIBUSB_BADARG;
	}

	usbi_debug(NULL, 4, "class=%d, subclass=%d, protocol=%d",
		devclass, subclass, protocol);

	*num_devids = 0;
	*devids = NULL;
	list_init(&match_list);

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if ((devclass < -1) || (devclass > 0xff) || (subclass < -1) ||
		(subclass > 0xff) || (protocol < -1) || (protocol > 0xff))
		return LIBUSB_BADARG;

	pthread_mutex_lock(&usbi_devices.lock);
	list_for_each_entry_safe(idev, tdev, &usbi_devices.head, dev_list) {

		pthread_mutex_unlock(&usbi_devices.lock);

		if (usbi_match_class(handle, idev, devclass, subclass,
					protocol)) {

			usbi_debug(NULL, 4, "match dev %d",
				(int)idev->devid);

			list_add(&idev->match_list, &match_list);
			(*num_devids)++;
		}

		pthread_mutex_lock(&usbi_devices.lock);
	}

	if (*num_devids == 0) {
		pthread_mutex_unlock(&usbi_devices.lock);
		return LIBUSB_NULL_LIST;
	}

	*devids = malloc((*num_devids) * sizeof (libusb_devid_t));
	if (*devids == NULL) {
		pthread_mutex_unlock(&usbi_devices.lock);
		return LIBUSB_NO_RESOURCES;
	}

	tdevid = *devids;
	list_for_each_entry(idev, &match_list, match_list) {
	/* safe */
		*tdevid = idev->devid;
		tdevid++;
	}
	pthread_mutex_unlock(&usbi_devices.lock);

	return LIBUSB_SUCCESS;
}

void libusb_free_devid_list(libusb_devid_t *devids)
{
	if (devids == NULL)
		return;

	free(devids);
}

#if 0
/* return -1, when status != 0 or ret < 0 */
int usbi_control_msg(libusb_dev_handle_t dev, int requesttype, int request,
	int value, int index, char *bytes, int size, int timeout)
{
	libusb_ctrl_request_t ctrl;
	int ret;
	
	if (size < 0) {
	/* NULL buffer is allowed for some CTRL xfer */
		return -1;
	}
	usbi_debug(NULL, 1, "Begin: type = %d, request=%d, index= %d",
		requesttype, request, index);

	memset(&ctrl, 0, sizeof(ctrl));
	
	ctrl.setup.bmRequestType = requesttype;
	ctrl.setup.bRequest = request;
	ctrl.setup.wValue = value;
	ctrl.setup.wIndex = index;

	ctrl.payload = bytes;
	ctrl.length = size;
	ctrl.timeout = timeout;

	ret = libusb_ctrl_xfer(dev, 0, 0, &ctrl);

	if (ret < 0 || ctrl.result.status != 0) {
		return -1;
	}
	return 0;
}

int usbi_get_descriptors(libusb_dev_handle_t dev, int type, int index,
	char *buf, int size)
{
	 return(usb_control_msg(dev,
			USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
			(type << 8) + index, 0, buf, size, 1000));
}
#endif

/* Descriptor operations 
 * Get raw descriptors of specified type/index 
 * buffer is allocated by libusb, buffer length is returned in buflen
 */
int32_t libusb_get_raw_desc(libusb_handle_t handle,
	libusb_devid_t devid, uint8_t type, uint8_t descidx,
	uint16_t langid, uint8_t **buffer, uint16_t *buflen)
{
	struct usbi_handle *hdl;
	struct usbi_device *idev;
	int ret;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	idev = usbi_find_device_by_id(devid);
	if (!idev)
		return LIBUSB_UNKNOWN_DEVICE;
	
	/*
	 * backends should implement get_raw_desc interface. The get_raw_desc
	 * method is OS dependent. On some OS, it's not easy to get  device's
	 * descriptor through CTRL endpoint.
	 */
	if(idev->ops->get_raw_desc) {

		ret = idev->ops->get_raw_desc(idev, type, descidx, langid,
				buffer, buflen);
		return ret;
	} else {
		return LIBUSB_PARSE_ERROR;
	}
}

void libusb_free_raw_desc(uint8_t *buffer)
{
	if (buffer == NULL)
		return;

	free(buffer);
}

int32_t libusb_parse_device_desc(libusb_handle_t handle,
	libusb_devid_t devid, uint8_t *buffer, uint16_t buflen,
	usb_device_desc_t *devdesc)
{
	struct usbi_handle *hdl;
	uint8_t *tmpbuf;
	uint16_t tmplen;
	int ret = LIBUSB_SUCCESS;
	size_t count;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;
	
	usbi_debug(hdl, 4, "devid = %d", (int)devid);

	if (buffer == NULL) {
		/* need to get raw desc internally */
		ret = libusb_get_raw_desc(handle, devid, USB_DESC_TYPE_DEVICE,
			0, 0, &tmpbuf, &tmplen);
		if (ret < 0) {
			usbi_debug(NULL, 1, "fail:%s", libusb_strerror(ret));
			return ret;
		}
	} else {
		if (buflen < USBI_DEVICE_DESC_SIZE)
			return LIBUSB_BADARG;

		tmpbuf = buffer;
		tmplen = buflen;
	}

	ret = libusb_parse_data("bbwbbbbwwwbbbb", tmpbuf, tmplen, devdesc,
		sizeof (usb_device_desc_t), &count);

	if ((ret == 0) && (count < USBI_DEVICE_DESC_SIZE))
		ret = LIBUSB_PARSE_ERROR;

	if (buffer == NULL)
		libusb_free_raw_desc(tmpbuf);

	return ret;
}

int32_t libusb_parse_config_desc(libusb_handle_t handle,
	libusb_devid_t devid, uint8_t *buffer, uint16_t buflen,
	uint8_t cfgidx, usb_config_desc_t *cfgdesc)
{
	struct usbi_handle *hdl;
	uint8_t *tmpbuf;
	uint16_t tmplen;
	int ret = LIBUSB_SUCCESS;
	size_t count;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if (buffer == NULL) {
		/* need to get raw desc internally */
		ret = libusb_get_raw_desc(handle, devid, USB_DESC_TYPE_CONFIG,
			cfgidx, 0, &tmpbuf, &tmplen);
		if (ret < 0)
			return ret;
	} else {
		if (buflen < USBI_CONFIG_DESC_SIZE)
			return LIBUSB_BADARG;

		tmpbuf = buffer;
		tmplen = buflen;
	}

	ret = libusb_parse_data("bbwbbbbb", tmpbuf, tmplen, cfgdesc,
		sizeof (usb_config_desc_t), &count);

	if ((ret == 0) && (count < USBI_CONFIG_DESC_SIZE))
		ret = LIBUSB_PARSE_ERROR;

	if (buffer == NULL)
		libusb_free_raw_desc(tmpbuf);

	return ret;
}

int32_t libusb_parse_interface_desc(libusb_handle_t handle,
	libusb_devid_t devid, uint8_t *buffer, uint16_t buflen,
	uint8_t cfgidx, uint8_t ifcidx, uint8_t alt,
	usb_interface_desc_t *ifcdesc)
{
	struct usbi_handle *hdl;
	uint8_t *tmpbuf, *sp;
	uint16_t tmplen;
	int ret = LIBUSB_PARSE_ERROR;
	size_t count;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if (buffer == NULL) {
		/* need to get raw desc internally */
		ret = libusb_get_raw_desc(handle, devid, USB_DESC_TYPE_CONFIG,
			cfgidx, 0, &tmpbuf, &tmplen);
		if (ret < 0)
			return ret;
	} else {
		if (buflen < USBI_CONFIG_DESC_SIZE)
			return LIBUSB_BADARG;

		tmpbuf = buffer;
		tmplen = buflen;
	}
	
	ret = LIBUSB_PARSE_ERROR; /* otherwise, the remaining will return
				   * success even if it never find a match
				   * interface
				   */
	sp = tmpbuf;
	while (tmplen > 3) {
		if ((sp[1] == USB_DESC_TYPE_INTERFACE) &&
			(sp[2] == ifcidx) &&
			(sp[3] == alt)) {
			ret = libusb_parse_data("bbbbbbbbb", sp, tmplen,
				ifcdesc, sizeof (usb_interface_desc_t), &count);
			if ((ret == 0) && (count < USBI_INTERFACE_DESC_SIZE))
				ret = LIBUSB_PARSE_ERROR;
			break;
		}

		if ((sp[0] > 0) && (tmplen >= sp[0])) {
			tmplen -= sp[0];
			sp += sp[0];
		} else {
			ret = LIBUSB_PARSE_ERROR;
			break;
		}
	}

	if (buffer == NULL)
		libusb_free_raw_desc(tmpbuf);

	return ret;
}

/* find the address of nth desc of a specified type */
static uint8_t *usbi_nth_desc(uint8_t *buffer, uint16_t buflen, uint8_t type,
	uint8_t n, uint8_t stop_type)
{
	uint8_t *sp =buffer;

	while (buflen >= 2) {
		if ((sp != buffer) && (sp[1] == stop_type))
			return NULL;

		if (sp[1] == type) {
			if ((n--) == 0)
				return sp;
		}

		if ((sp[0] == 0) || (buflen < sp[0]))
			return NULL;

		sp += sp[0];
		buflen -= sp[0];
	}
	
	return NULL;
}

int32_t libusb_parse_endpoint_desc(libusb_handle_t handle,
	libusb_devid_t devid, uint8_t *buffer, uint16_t buflen,
	uint8_t cfgidx, uint8_t ifcidx, uint8_t alt, uint8_t eptidx,
	usb_endpoint_desc_t *eptdesc)
{
	struct usbi_handle *hdl;
	uint8_t *tmpbuf, *sp1, *sp2;
	uint16_t tmplen;
	int ret = LIBUSB_PARSE_ERROR;
	size_t count;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if (buffer == NULL) {
		/* need to get raw desc internally */
		ret = libusb_get_raw_desc(handle, devid, USB_DESC_TYPE_CONFIG,
				cfgidx, 0, &tmpbuf, &tmplen);
		if (ret < 0) {
			usbi_debug(hdl, 1, "Get raw fail:%s",
				libusb_strerror(ret));
			return ret;
		}
	} else {
		if (buflen < USBI_CONFIG_DESC_SIZE) {
			usbi_debug(hdl, 1, "Invalid buffer length");
			return LIBUSB_BADARG;
		}

		tmpbuf = buffer;
		tmplen = buflen;
	}

	sp1 = tmpbuf;
	while (tmplen > 4) {
		if ((sp1[1] == USB_DESC_TYPE_INTERFACE) &&
			(sp1[2] == ifcidx) &&
			(sp1[3] == alt)) {

			if (eptidx >= sp1[4]) {
				usbi_debug(hdl, 1, "Invalid endpoint:%d",eptidx);
				ret = LIBUSB_BADARG;
				break;
			}

			sp2 = sp1;
			if ((sp1 = usbi_nth_desc(sp1, tmplen,
				USB_DESC_TYPE_ENDPOINT, eptidx,
				USB_DESC_TYPE_INTERFACE)) == NULL) {

				ret = LIBUSB_PARSE_ERROR;
				break;
			}
			tmplen -= (sp1 - sp2);

			ret = libusb_parse_data("bbbbwb", sp1, tmplen,
				eptdesc, sizeof (usb_endpoint_desc_t), &count);
			if ((ret == 0) && (count < USBI_ENDPOINT_DESC_SIZE))
				ret = LIBUSB_PARSE_ERROR;
			break;
		}

		if ((sp1[0] > 0) && (tmplen >= sp1[0])) {
			tmplen -= sp1[0];
			sp1 += sp1[0];
		} else {
			ret = LIBUSB_PARSE_ERROR;
			break;
		}
	}

	if (buffer == NULL)
		libusb_free_raw_desc(tmpbuf);

	return ret;
}


int usbi_get_string(libusb_dev_handle_t dev, int index, int langid, char *buf,
    size_t buflen)
{
	libusb_ctrl_request_t ctrl;
	
	/* if index == 0, then the caller wants to get STRING descript Zero
	 * for LANGIDs
	 */
	if ((buf == NULL) || (buflen == 0)) {
		usbi_debug(NULL,1,
			"usbi_get_string(): NULL handle or data");

		return LIBUSB_BADARG;
	}

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.setup.bmRequestType = USB_REQ_DEV_TO_HOST;
	ctrl.setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	ctrl.setup.wValue = (USB_DESC_TYPE_STRING << 8) + index;
	ctrl.setup.wIndex = langid;
	ctrl.payload = (uint8_t *)buf;
	ctrl.length = buflen;
	ctrl.timeout = 100;

	usbi_debug(NULL, 4,
		"usbi_get_string(): index=%d langid=0x%x len=%d",
		index, langid, buflen);


	if(libusb_ctrl_xfer(dev, 0, 0, &ctrl) == 0) {
		return ctrl.result.transferred_bytes;
	} else {
		return -1;
	}
}

/* return value:
 * 	> 0 string character numbers 
 *      < 0 errors
 */
int usbi_get_string_simple(libusb_dev_handle_t dev, int index, char *buf,
    size_t buflen)
{
	char tbuf[256];
	int ret, langid, si, di;

	usbi_debug(NULL, 4, "usb_get_string_simple(): index=%d", index);
	
	if (index == 0) {
		usbi_debug(NULL, 1, "not a valid string index");
		return (LIBUSB_BADARG);
	}

	if ((buf == NULL) || (buflen == 0)) {
		usbi_debug(NULL, 1,
			"usbi_get_string_simple(): NULL handle or data\n");

		return (LIBUSB_BADARG);
	}

	(void) memset(buf, 0, buflen);

	/*
	 * Asking for the zero'th index is special - it returns a string
	 * descriptor that contains all the language IDs supported by the
	 * device. Typically there aren't many - often only one. The
	 * language IDs are 16 bit numbers, and they start at the third byte
	 * in the descriptor. See USB 2.0 specification, section 9.6.7, for
	 * more information on this.
	 */
	ret = usbi_get_string(dev, 0, 0, tbuf, sizeof (tbuf));
	usbi_debug(NULL, 4, "usbi_get_string() first returned %d", ret);

	if (ret < 4) {
		langid = 0x409;
	} else {
		langid = tbuf[2] | (tbuf[3] << 8);
	}

	ret = usbi_get_string(dev, index, langid, tbuf, sizeof (tbuf));

	usbi_debug(NULL, 4 , "usbi_get_string() returned %d", ret);

	if (ret < 0) {

		return (ret);
	}
	if (tbuf[1] != USB_DESC_TYPE_STRING) {

		return (-EIO);
	}
	if (tbuf[0] > ret) {

		return (-EFBIG);
	}

	for (di = 0, si = 2; si < tbuf[0]; si += 2) {
		if (di >= (buflen - 1)) {
			break;
		}

		if (tbuf[si + 1]) {  /* high byte */
			buf[di++] = '?';
		} else {
			buf[di++] = tbuf[si];
		}
	}

	buf[di] = 0;

	usbi_debug(NULL, 4 , "usbi_get_string() returned %s", buf);
	return (di);
}


/* TODO */
int32_t libusb_get_device_data(libusb_handle_t handle, libusb_devid_t devid,
	uint32_t flags, libusb_dev_data_t **data)
{
	libusb_dev_data_t *pdata;
	struct usbi_handle *plib;
	struct usbi_device *pdev;
	uint16_t datalen;
	uint8_t *descdata;
	int ret;
	char strings[256];
	libusb_dev_handle_t hdev;
	struct usbi_dev_handle *devh, *dev_found=NULL;

	usbi_debug(NULL, 4, "devid=%d, flags=%d",(int)devid, flags);

	plib = usbi_find_handle(handle);
	if(!plib) {
		usbi_debug(NULL, 1, "Can't find lib handle:%d ",handle);
		return LIBUSB_BADARG;
	}

	pdev = usbi_find_device_by_id(devid);
	if(!pdev) {
		usbi_debug(NULL, 1, "Can't find device:%d ",devid);
		return LIBUSB_BADARG;
	}


	pdata = malloc(sizeof(libusb_dev_data_t));
	if(!pdata) {
		return LIBUSB_NO_RESOURCES;
	}
	memset(pdata, 0, sizeof(*pdata));

	pdata->bulk_max_xfer_size = pdev->bus->max_xfer_size[USB_TYPE_BULK];
	pdata->ctrl_max_xfer_size = pdev->bus->max_xfer_size[USB_TYPE_CONTROL];
	pdata->intr_max_xfer_size = pdev->bus->max_xfer_size[USB_TYPE_INTERRUPT];
	pdata->isoc_max_xfer_size = pdev->bus->max_xfer_size[USB_TYPE_ISOCHRONOUS];

	pdata->busid = pdev->bus->busid;
	pdata->bus_address = pdev->bus->busnum;

	/* since we're not allowed to cache device data internally,we'll have
	 * to get raw descriptors
	 */
	ret = libusb_parse_device_desc(handle, devid, NULL, 0, &pdata->dev_desc);
	if (ret != 0) {
		usbi_debug(NULL, 1,"Get device desc fail");
		free(pdata);
		return ret;
	}

	if (pdata->dev_desc.iManufacturer == 0 && pdata->dev_desc.iProduct == 0
		&& pdata->dev_desc.iSerialNumber == 0) {
		usbi_debug(NULL, 4, "Don't have string descriptors");
		goto get_raw;
	}

#if 1
	/* get manufacturer, product, serialnumber strings
	 * Use 0x0409 US English as default LANGID. To get other
	 * language strings, use libusb_get_raw_desc instead.
	 */

	/* find if we have already opened this device
	 * We have to access an opened device to get_string
	 * FIXME: what about just re-opening it no matter it's
	 *	opened or not.
	 */
	pthread_mutex_lock(&usbi_dev_handles.lock);
	list_for_each_entry(devh, &usbi_dev_handles.head, list) {
	/* safe */
		if (devh->idev->devid == devid) {
			dev_found = devh;
			break;
		}
	}
	pthread_mutex_unlock(&usbi_dev_handles.lock);

	if (!dev_found) {
	/* not opened yet */
		usbi_debug(NULL, 4, "device not opened");

		ret = libusb_open_device(handle, devid, 0, &hdev);

		if (ret == LIBUSB_NOT_SUPPORTED) {
		/* this device, like root-hub, don't support get
		 * descriptors, but it can provide other data. 
		 * So we still proceed
		 */
		 	usbi_debug(NULL, 3, "Not support strings");
			pdata->manufacturer = NULL;
			pdata->product = NULL;
			pdata->serialnumber = NULL;
		 	goto get_raw;

		} else if (ret != 0) {

			usbi_debug(NULL, 1, "Fail to open device");

			free(pdata);
			return LIBUSB_PLATFORM_FAILURE;
		}
	} else {
		usbi_debug(NULL, 4, "device already opened");
		hdev = dev_found->handle;
	}

	/* manufacturer */
	if (pdata->dev_desc.iManufacturer) {
		usbi_debug(NULL, 1, "get manufacturer");
		if ((ret = usbi_get_string(hdev, pdata->dev_desc.iManufacturer,
			0x409, strings, sizeof(strings))) < 0) {
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return ret;
		}

		if ((pdata->manufacturer = malloc(strings[0])) == NULL) {
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return LIBUSB_NO_RESOURCES;
		}
		memcpy(pdata->manufacturer, strings, strings[0]);
	}

	/* product */
	if (pdata->dev_desc.iProduct) {
		usbi_debug(NULL, 1, "get product");
		if ((ret = usbi_get_string(hdev, pdata->dev_desc.iProduct,
			0x409, strings, sizeof(strings))) < 0) {
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return ret;
		}

		if ((pdata->product= malloc(strings[0])) == NULL) {
			free(pdata->manufacturer);
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return LIBUSB_NO_RESOURCES;
		}
		memcpy(pdata->product, strings, strings[0]);
	}

	/* serial Number */
	if (pdata->dev_desc.iSerialNumber) {
		if ((ret = usbi_get_string(hdev, pdata->dev_desc.iSerialNumber,
			0x409, strings, sizeof(strings))) < 0) {
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return ret;
		}

		if ((pdata->serialnumber = malloc(strings[0])) == NULL) {
			free(pdata->product);
			free(pdata->manufacturer);
			free(pdata);
			if (!dev_found) { libusb_close_device(hdev); }
			return LIBUSB_NO_RESOURCES;
		}

		memcpy(pdata->serialnumber, strings, strings[0]);
	}
	
	if (!dev_found) {
		libusb_close_device(hdev);
	}
#endif

get_raw:
	ret = libusb_get_raw_desc(handle, devid, USB_DESC_TYPE_CONFIG,
			pdev->cur_config, 0, &descdata, &datalen);
	if (ret != 0) {
		usbi_debug(NULL, 1, "Get raw config(%d) desc fail",
			pdev->cur_config);
		goto fail;
	}

	ret = libusb_parse_config_desc(handle, devid, descdata, datalen,
			pdev->cur_config, &pdata->cfg_desc);
	if (ret != 0) {
		usbi_debug(NULL, 1, "Parse config fail");
		goto fail;
	}
	usbi_debug(NULL, 4, "data len = %d",datalen);

	pdata->raw_cfg_desc = malloc(datalen);
	if (!pdata->raw_cfg_desc) {
		libusb_free_raw_desc(descdata);
		ret = LIBUSB_NO_RESOURCES;
		goto fail;
	}

	memcpy(pdata->raw_cfg_desc, descdata, datalen);

	libusb_free_raw_desc(descdata);

	/* topological path such as 1.2.1 */
	pdata->bus_path = strdup(pdev->bus_path);

	pdata->sys_path = strdup(pdev->sys_path);

	pdata->devid = devid;
	pdata->nports = pdev->nports;
	pdata->pdevid = (pdev->parent)?pdev->parent->devid:0;/* 0 for root hub */
	pdata->pport = pdev->pport;

	*data = pdata;
	return 0;

fail:
	free(pdata->product);
	free(pdata->manufacturer);
	free(pdata->serialnumber);
	free(pdata);
	return ret;
}

void libusb_free_device_data(libusb_dev_data_t *data)
{
	if (data == NULL)
		return;
	
	if (data->raw_cfg_desc) {
		free(data->raw_cfg_desc);
	}

	if(data->product) {
		free(data->product);
	}

	if (data->manufacturer) {
		free(data->manufacturer);
	}

	if (data->serialnumber) {
		free(data->serialnumber);
	}
	
	free(data->bus_path);
	free(data->sys_path);
	free(data);
}

int32_t libusb_get_max_xfer_size(libusb_handle_t handle,
	libusb_busid_t bus, libusb_transfer_type_t type, uint32_t *bytes)
{
	struct usbi_bus			*ibus;
	struct usbi_handle	*hdl;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return (LIBUSB_INVALID_HANDLE);

	ibus = usbi_find_bus_by_id(bus);
	if (!ibus)
		return (LIBUSB_UNKNOWN_DEVICE);

	if ((type <= USB_TYPE_ALL) || (type >= USB_TYPE_LAST)) {
		usbi_debug(hdl,2,"Invalid transfer type");
		return (LIBUSB_BADARG);
	}

	if (bytes == NULL) {
		return (LIBUSB_BADARG);
	}

	/* return our value */
	*bytes = ibus->max_xfer_size[type];

	return (LIBUSB_SUCCESS);
}
