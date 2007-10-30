/*
 * API implementation
 *
 * Copyright (c) 2007 Sun Microsystems, Inc. All rights reserved 
 * Use is subject to license terms.
 *
 * Copyright 2006 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */
#include <string.h>
#include <pthread.h>
#include "usbi.h"

#define USB_EP_TYPE_MASK 0x03

/*
 * FIXME: this func can only be called after calling 
 * libusb_set_event_callback()? Not quite understand what this
 * func to do.
 */
void libusb_coldplug_callbacks_done(libusb_handle_t handle)
{
	struct usbi_handle *hdl;
	
	hdl = usbi_find_handle(handle);
	if (!hdl) {
		return;
	}

	pthread_mutex_lock(&hdl->lock);
	while (!hdl->coldplug_complete)
		pthread_cond_wait(&hdl->coldplug_cv, &hdl->lock);
	hdl->coldplug_complete = 0;
	pthread_mutex_unlock(&hdl->lock);
}

int32_t libusb_set_configuration(libusb_dev_handle_t dev, uint8_t cfg)
{
	struct usbi_dev_handle *hdev;
	usb_device_desc_t desc;
	int ret;

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;
	
	if ((ret = libusb_parse_device_desc(hdev->lib_hdl->handle,
		hdev->idev->devid, NULL, 0, &desc)) != 0) {
		return ret;
	}

	if(cfg < 1 || cfg > desc.bNumConfigurations) {
		return LIBUSB_BADARG;
	}

	return hdev->idev->ops->set_configuration(hdev, cfg);
}

int32_t libusb_get_configuration(libusb_dev_handle_t dev, uint8_t *cfg)
{
	struct usbi_dev_handle *hdev;
	if (!cfg) {
		return LIBUSB_BADARG;
	}
	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	return hdev->idev->ops->get_configuration(hdev, cfg);
}

int32_t libusb_claim_interface(libusb_dev_handle_t dev, uint8_t ifc,
	libusb_init_flag_t flags)
{
	struct usbi_dev_handle *hdev;
	int32_t ret;

	if (ifc > USBI_MAXINTERFACES) {
		return(LIBUSB_BADARG);
	}

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;
	
	/* refresh descriptors before we use it */
	if (usbi_fetch_and_parse_descriptors(hdev) != 0) {
		return LIBUSB_BADARG;
	}

	/* check if this is a valid interface */
	if ((ifc>= USBI_MAXINTERFACES) ||
	    (ifc >= hdev->idev->desc.configs[hdev->config_value-1].
	    		num_interfaces)) {

		usbi_debug(hdev->lib_hdl, 1, "interface %d not valid",
			ifc);

		return (LIBUSB_BADARG);
	}

	ret = hdev->idev->ops->claim_interface(hdev, ifc, flags);

	if(ret == 0) {
		hdev->claimed_ifs[ifc].clm= USBI_IFC_CLAIMED;
		hdev->claimed_ifs[ifc].altsetting = 0; /*set to default 0 */
	}
	return ret;
}

int32_t libusb_release_interface(libusb_dev_handle_t dev, uint8_t ifc)
{
	struct usbi_dev_handle *hdev;

	if (ifc > USBI_MAXINTERFACES) {
		return(LIBUSB_BADARG);
	}

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;
	
	if (libusb_is_interface_claimed(dev, ifc) != 1) {
		return LIBUSB_BADARG;
	}

	return hdev->idev->ops->release_interface(hdev, ifc);
}

int32_t libusb_is_interface_claimed(libusb_dev_handle_t dev, uint8_t ifc)
{
	struct usbi_dev_handle *hdev;

	if(ifc > USBI_MAXINTERFACES) {
		return LIBUSB_BADARG;
	}
	
	hdev = usbi_find_dev_handle(dev);

	if(!hdev) {
		return LIBUSB_BADARG;
	}

	if (hdev->claimed_ifs[ifc].clm == USBI_IFC_CLAIMED) {
		return 1;
	} else {
		return 0;
	}
}

int32_t libusb_set_altsetting(libusb_dev_handle_t dev, uint8_t ifc,
	uint8_t alt)
{
	struct usbi_dev_handle *hdev;
	struct usbi_device *idev;
	struct usbi_config *pcfg;

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	if (ifc > USBI_MAXINTERFACES) {
		return LIBUSB_BADARG;
	}

	idev = hdev->idev;
	
	/* refresh descriptors */
	if (usbi_fetch_and_parse_descriptors(hdev) != 0) {
		return LIBUSB_PARSE_ERROR;
	}

	pcfg=&idev->desc.configs[hdev->config_value - 1];

	/* not valid interface, or not claimed, or not valid alt */
	if (ifc > pcfg->num_interfaces || ifc > USBI_MAXINTERFACES
		|| hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED 
		|| alt >= pcfg->interfaces[ifc].num_altsettings ) {
		/* alternate counts from 0 */
		usbi_debug(hdev->lib_hdl, 1,
			"invalid interface(%d) or alt(%d)", ifc, alt);
		return LIBUSB_BADARG;
	}

	if (alt == hdev->claimed_ifs[ifc].altsetting) {
		usbi_debug(hdev->lib_hdl, 1, "same alt, no need to change");

		return (0);
	}

	return hdev->idev->ops->set_altsetting(hdev, ifc, alt);
}

int32_t libusb_get_altsetting(libusb_dev_handle_t dev, uint8_t ifc,
	uint8_t *alt)
{
	struct usbi_device *idev;
	struct usbi_dev_handle *hdev;

	if (!alt || (ifc > USBI_MAXINTERFACES)) {
		return LIBUSB_BADARG;
	}

	hdev=usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	/* not claimed */
	if (hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED) {
		return LIBUSB_BADARG;
	}

	idev = hdev->idev;

	return idev->ops->get_altsetting(hdev, ifc, alt);
}

int32_t libusb_reset(libusb_dev_handle_t dev)
{
	struct usbi_dev_handle *hdev;

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	if (!(hdev->idev->ops->reset)) {
		return LIBUSB_NOT_SUPPORTED;
	}
	return hdev->idev->ops->reset(hdev);
}

int32_t usbi_control_xfer(struct usbi_dev_handle *devh,int requesttype,
        int request, int value, int index, char *bytes, int size, int timeout)
{
	libusb_ctrl_request_t ctrl;
	struct libusb_request_handle req;
	int ret;

	memset(&ctrl, 0, sizeof(ctrl));
	memset(&req, 0, sizeof(req));

	ctrl.setup.bmRequestType = requesttype;
	ctrl.setup.bRequest = request;
	ctrl.setup.wValue = value;
	ctrl.setup.wIndex = index;

	ctrl.payload = (uint8_t *)bytes;
	ctrl.length = size;
	ctrl.timeout = timeout;

	req.dev = devh->handle;
	req.interface = 0;
	req.endpoint = 0;
	req.type  = USB_TYPE_CONTROL;
	req.req.ctrl = &ctrl;

	ret = usbi_io_sync(devh, &req);
	if (ret < 0) {
		usbi_debug(NULL, 1, "control xfer fail");
	}

	return ret;
}

int32_t usbi_get_config_desc(struct usbi_dev_handle *devh, int cfg, char **cfgbuf,
                int32_t *cfglen)
{
	char buf[8];
	char *newbuf;
	int ret;
	struct usb_config_desc cfg_desc;
	int count;

	ret = usbi_control_xfer(devh, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
			(USB_DESC_TYPE_CONFIG << 8) + cfg, 0, buf,
			8, 100);
	if (ret < 0) {
		usbi_debug(NULL, 1, "usbi_control_xfer fail");
		return ret;
	}

	libusb_parse_data("bbw", (unsigned char *)buf, 8, &cfg_desc,
			sizeof(cfg_desc), (size_t *)&count);

	newbuf = malloc(cfg_desc.wTotalLength);
	if (!newbuf) {
		usbi_debug(NULL, 1, "no memory");
		return LIBUSB_NO_RESOURCES;
	}

	ret = usbi_control_xfer(devh, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
			(USB_DESC_TYPE_CONFIG << 8) + cfg, 0, newbuf,
			cfg_desc.wTotalLength, 100);

	if (ret < 0) {
		free(newbuf);
		usbi_debug(NULL, 1, "usbi_control_xfer fail");
		return ret;
	}

	*cfgbuf = newbuf;
	*cfglen = cfg_desc.wTotalLength;

	usbi_debug(NULL, 4, "End");

	return 0;
}

void usbi_free_cfg(char *buf)
{
	if (buf == NULL) {
		return;
	}

	free(buf);
}

/*
 * Check if the interface has claimed, the endpoint address is correct,
 * transfer type matched endpoint attribute.
 * But, we can't judge if a READ request is sent to an out endpoint, and
 * vice vesa.
 */
int32_t check_req_valid(libusb_request_handle_t req, 
	struct usbi_dev_handle *dev)
{
	libusb_transfer_type_t type = req->type;
	uint8_t ifc = req->interface;
	uint8_t endpoint = req->endpoint;
	uint8_t cfg;
	uint8_t alt;
	int ret = 0;
	usb_endpoint_desc_t *pdesc=NULL;
	int i;
	usb_interface_desc_t if_desc;
	usb_endpoint_desc_t ep_desc;
	char *buf;
	int  buflen;

	if ((endpoint == 0) && (type == USB_TYPE_CONTROL)) {
	/* default pipe. No need to check interface,altSetting */
		return 0;
	} else if (endpoint == 0) {
	/* default endpoint,but not a CONTROL xfer request */
		return -1;
	}

	ret = libusb_get_configuration(dev->handle, &cfg);
	if(ret < 0) {
		usbi_debug(dev->lib_hdl, 1, "fail get current config");
		return ret;
	}

	/* implicit check of interface claiming */
	ret = libusb_get_altsetting(dev->handle,ifc,&alt);
	if (ret < 0) {
		usbi_debug(dev->lib_hdl, 1, "fail get current altsetting");
		return ret;
	}
	
	/*
	 * cfg is the current config value, not the index 
	 * endpoint index is not bEndpointAddress
	 */
	
	/*
	 * Since we are not allowed to use cached descriptors, we have to
	 * get descriptors from device everytime we need it. Seems very low
	 * efficient! Get descriptor from endpoint 0, not by get_descr_raw()
	 */
	ret = usbi_get_config_desc(dev, cfg, &buf, &buflen);
	if (ret < 0) {
		usbi_debug(NULL, 1, "get raw descriptor fail");
		return ret;
	}

	/* this interface requires config index */
	ret = libusb_parse_interface_desc(dev->lib_hdl->handle,
		dev->idev->devid, (uint8_t *)buf, buflen, cfg-1, ifc, alt,
		&if_desc);

	if (ret < 0) {
		usbi_free_cfg(buf);
		usbi_debug(dev->lib_hdl, 1, "parse interface desc error");
		return ret;
	}

	for (i = 0; i< if_desc.bNumEndpoints; i++) {

		/* this interface requires config index */
		ret = libusb_parse_endpoint_desc(dev->lib_hdl->handle,
			dev->idev->devid, (uint8_t *)buf, buflen, cfg-1, ifc, alt,
			i, &ep_desc);
		if (ret < 0) {
			usbi_free_cfg(buf);
			usbi_debug(dev->lib_hdl, 1,
				"parse endpoint desc error");
			return ret;
		}

		if (endpoint == ep_desc.bEndpointAddress) {
			break;
		}
	}

	if (i == if_desc.bNumEndpoints) {
		/* not find an endpoint with EndpointAddress == endpoint */
		usbi_debug(dev->lib_hdl, 1, "Invalid endpoint in request");
		usbi_free_cfg(buf);
		return(LIBUSB_INVALID_HANDLE);
	}

	pdesc = &ep_desc;

	switch(type) {
		case USB_TYPE_CONTROL:
			if((pdesc->bmAttributes & USB_EP_TYPE_MASK) != 0){
				/*Request a CTRL xfer on a non-Ctrl endpoint */
				usbi_debug(dev->lib_hdl, 1, "invalid type");
				ret = LIBUSB_INVALID_HANDLE;
			}
			break;
		case USB_TYPE_INTERRUPT:
			if ((pdesc->bmAttributes & USB_EP_TYPE_MASK) != 3) {
				/* Request INTR xfer on a non-INTR endpoint */
				usbi_debug(dev->lib_hdl, 1, "invalid type");
				ret = LIBUSB_INVALID_HANDLE;
			}
			break;
		case USB_TYPE_BULK:
			if ((pdesc->bmAttributes & USB_EP_TYPE_MASK) != 2) {
				/* Request BULK xfer on a non-BULK endpoint */
				usbi_debug(dev->lib_hdl, 1, "invalid type");
				ret = LIBUSB_INVALID_HANDLE;
			}
			break;
		case USB_TYPE_ISOCHRONOUS:
			if ((pdesc->bmAttributes & USB_EP_TYPE_MASK) != 1) {
				/* Request ISOC xfer on a non-ISOC endpoint */
				usbi_debug(dev->lib_hdl, 1, "invalid type");
				ret = LIBUSB_INVALID_HANDLE;
			}
			break;
		default:
			usbi_debug(dev->lib_hdl, 1, "unknown type");
			ret = LIBUSB_INVALID_HANDLE;
			break;
	}

	usbi_free_cfg(buf);
	return ret;
}

int32_t libusb_xfer_wait(libusb_request_handle_t req)
{
	struct usbi_dev_handle *dev=NULL;
	int32_t ret;
	int32_t	io_pattern;

	usbi_debug(NULL, 4, "Begin: ifc=%d ept=%x type=%d", req->interface,
		req->endpoint, req->type);

	if(!req) {
		usbi_debug(NULL, 1, "Invalid request");
		return LIBUSB_BADARG;
	}

	dev = usbi_find_dev_handle(req->dev); 
	if (!dev) {
		usbi_debug(NULL, 1, "Can't find device handle:%llu",req->dev);
		return LIBUSB_INVALID_HANDLE;
	}
	
	if (check_req_valid(req, dev) < 0) {
		usbi_debug(dev->lib_hdl, 1, "Not a valid request");
		return LIBUSB_BADARG;
	}

  	io_pattern = dev->idev->bus->ops->io_pattern;
	if (io_pattern < PATTERN_ASYNC || io_pattern > PATTERN_BOTH) {
		return LIBUSB_PLATFORM_FAILURE;
	}

	/* FIXME: add more check for request validatin */
	/* for INTR,BULK,ISOC EPs:
	 *    OUT - if data length > 0 and buf=NULL, then it's invalid
	 *    IN  - if buf=NULL, then invalid
	 */

	ret = usbi_io_sync(dev, req);
	if (ret < 0) {
		return LIBUSB_PLATFORM_FAILURE;
	}

	ret = LIBUSB_SUCCESS;

	return ret;
}

int32_t libusb_ctrl_xfer(libusb_dev_handle_t dev, uint8_t ifc, uint8_t ept,
	libusb_ctrl_request_t *ctrl)
{
	libusb_request_handle_t reqp;
	int32_t ret;

	if (ctrl == NULL) {
		return LIBUSB_BADARG;
	}
	
	usbi_debug(NULL, 4, "ifc=%d ept=%d bRequest=%d", ifc, ept,
		ctrl->setup.bRequest);

	reqp = calloc(sizeof(struct libusb_request_handle), 1);
	if(reqp == NULL) {
		return LIBUSB_NO_RESOURCES;
	}

	reqp->dev = dev;
	reqp->interface = ifc;
	reqp->endpoint = ept;
	reqp->type  = USB_TYPE_CONTROL;
	reqp->req.ctrl = ctrl;

	ret = libusb_xfer_wait(reqp);

	free(reqp);

	return ret;
}

int32_t libusb_intr_xfer(libusb_dev_handle_t dev,uint8_t ifc, uint8_t ept,
	libusb_intr_request_t *intr)
{
	libusb_request_handle_t reqp;
	int32_t ret;

	if(intr == NULL) {
		return LIBUSB_BADARG;
	}

	reqp = calloc(sizeof(struct libusb_request_handle), 1);
	if (reqp == NULL) {
		return LIBUSB_NO_RESOURCES;
	}

	reqp->dev = dev;
	reqp->interface = ifc;
	reqp->endpoint = ept;
	reqp->type = USB_TYPE_INTERRUPT;
	reqp->req.intr = intr;

	ret = libusb_xfer_wait(reqp);
	free(reqp);

	return ret;
}

int32_t libusb_bulk_xfer(libusb_dev_handle_t dev,uint8_t ifc, uint8_t ept,
	libusb_bulk_request_t *bulk)
{
	libusb_request_handle_t reqp;
	int32_t ret;

	if(bulk == NULL) {
		return LIBUSB_BADARG;
	}

	reqp = calloc(sizeof(struct libusb_request_handle), 1);
	if (reqp == NULL) {
		return LIBUSB_NO_RESOURCES;
	}

	reqp->dev = dev;
	reqp->interface = ifc;
	reqp->endpoint = ept;
	reqp->type = USB_TYPE_BULK;
	reqp->req.bulk = bulk;

	ret = libusb_xfer_wait(reqp);
	free(reqp);

	return ret;
}

int32_t libusb_isoc_xfer(libusb_dev_handle_t dev,uint8_t ifc, uint8_t ept,
	libusb_isoc_request_t *isoc)
{
	libusb_request_handle_t reqp;
	int32_t ret;
	
	if(isoc == NULL) {
		return LIBUSB_BADARG;
	}

	reqp = calloc(sizeof(struct libusb_request_handle), 1);
	if (reqp == NULL) {
		return LIBUSB_NO_RESOURCES;
	}

	reqp->dev = dev;
	reqp->interface = ifc;
	reqp->endpoint = ept;
	reqp->type = USB_TYPE_ISOCHRONOUS;
	reqp->req.isoc = isoc;

	ret = libusb_xfer_wait(reqp);
	free(reqp);

	return ret;
}


int32_t usbi_get_xfer_timeout(libusb_request_handle_t req, 
	struct usbi_dev_handle *dev)
{
	int32_t timeout;
	switch(req->type){
		case USB_TYPE_CONTROL:
			timeout = req->req.ctrl->timeout;
			break;
		case USB_TYPE_BULK:
			timeout = req->req.bulk->timeout;
			break;
		case USB_TYPE_INTERRUPT:
			timeout = req->req.intr->timeout;
			break;
		case USB_TYPE_ISOCHRONOUS:
			timeout = 0; /* FIXME: =0 ? */
			break;
		default:
			timeout = -1;
	}

	if(timeout == -1) {
		return -1;
	}

	if(timeout == 0) {
		timeout = dev->lib_hdl->timeout[req->type];
	}
	return timeout;
}


int32_t libusb_xfer_aio(libusb_request_handle_t req)
{
	int ret;
	struct usbi_dev_handle *dev;
	struct usbi_io *io;
	int32_t timeout;
	struct usbi_handle *plib;

	if (!req) {
		return LIBUSB_BADARG;
	}

	dev = usbi_find_dev_handle(req->dev); 
	if (!dev) {
		usbi_debug(NULL, 1, "Can't find device");
		return LIBUSB_BADARG;
	}

	ret = check_req_valid(req, dev);
	if (ret < 0) {
		usbi_debug(dev->lib_hdl, 1, "Invalid request");
		return LIBUSB_INVALID_HANDLE;
	}

	timeout = usbi_get_xfer_timeout(req, dev);

	io = usbi_alloc_io(dev, req, timeout);

	if (!io) {
		usbi_debug(dev->lib_hdl, 1, "IO alloc fail");
		return LIBUSB_NO_RESOURCES;
	}
	io->req = req;
	io->status = USBI_IO_INPROGRESS;
	io->flag = USBI_ASYNC;

	plib = dev->lib_hdl;

	ret = usbi_io_async(io);
	if(ret != 0) {
		usbi_debug(dev->lib_hdl, 1, "async fail: %s",
			libusb_strerror(ret));

		pthread_mutex_lock(&dev->lock);
		list_del(&io->list);	
		pthread_mutex_unlock(&dev->lock);

		usbi_free_io(io);
		return ret;
	}	

	usbi_debug(NULL, 4, "End");

	return 0;
}

/*
 * Don't set request's callback if this interface is used.
 */
int32_t libusb_wait(uint32_t num_reqs,libusb_request_handle_t *handles, 
	libusb_request_handle_t *handle)
{
	int i,found = 0;
	struct usbi_dev_handle *hdev;
	struct usbi_handle *ph;/* assuming all these request are in the same
				* libusb instance
				*/
	struct usbi_io *io;


	if (num_reqs == 0) {
	/* FIXME: shall we return success? */
		return 0;
	}

	if(!handles || !handle) {
		return LIBUSB_BADARG;
	}

	hdev = usbi_find_dev_handle(handles[0]->dev);
	if (!hdev) {
		usbi_debug(NULL, 1, "can't find device");
		return LIBUSB_BADARG;
	}

	ph = hdev->lib_hdl;

	if(!ph) {
		usbi_debug(NULL, 1, "lib handle error");
		return LIBUSB_BADARG;
	}

	/* if callback is set, then callback will be called
	 * immediately after IO is completed by backend. This IO
	 * is also freed at that moment. Thus may cause illegal
	 * pointer
	 */
	for(i = 0; i < num_reqs; i++) {
		if (handles[i]->cb != NULL) {
			usbi_debug(hdev->lib_hdl, 1, "Callback should not"
				"set here");
			return LIBUSB_BADARG;
		}
	}

waiting:
	pthread_mutex_lock(&ph->complete_lock);

	usbi_debug(hdev->lib_hdl, 4 ,"ph = %p, cv=%p, count = %d, lock=%p",ph,
		&ph->complete_cv, ph->complete_count,&ph->complete_lock);

	while (ph->complete_count == 0) {
		pthread_cond_wait(&(ph->complete_cv), &(ph->complete_lock));
	}

	list_for_each_entry(io, &ph->complete_list, list) {
	/* safe */
		if (io) {
			/*get the first completed io */
			usbi_debug(hdev->lib_hdl, 4, "waiting list: %p\n",
				io->req);

			for (i = 0; i < num_reqs; i++) {
				if(io->req == handles[i]) {
					found = 1;
					break;
				}
			}

			if (found == 1)
				break;
		}
	}

	if(found == 1) { /* find one on the complete list */
		list_del(&io->list); /* remove it from the list */

		ph->complete_count--;
		usbi_debug(hdev->lib_hdl, 4, "One was completed");

		*handle = io->req;
		pthread_mutex_unlock(&ph->complete_lock);
		usbi_free_io(io);
		return 0;

	} else { /* Maybe the submitted io not complete, waiting it */
		usbi_debug(hdev->lib_hdl, 4, "Continue waiting");

		/* FIXME:need more consideration. We may miss completed
		 * request here.
		 *
		 * if we don't subtract this count, then this loop will
		 * keep running even if the requested REQ is not completed
		 */
		ph->complete_count--; 

		pthread_mutex_unlock(&ph->complete_lock);
		goto waiting;
	}
	
	pthread_mutex_unlock(&ph->complete_lock);

	return 0;
}

int32_t libusb_poll(uint32_t num_reqs,libusb_request_handle_t * handles,
	libusb_request_handle_t * handle)
{
	int i;
	struct usbi_dev_handle *hdev;
	struct usbi_handle *ph=NULL;/* assuming all these request are in the same 
				     * libusb instance
				     */
	struct usbi_io *io=NULL;
	int found = 0;

	usbi_debug(NULL, 4, "Begin");

	if (num_reqs == 0) {
	/* FIXME: return success? */
		return 0;
	}

	if(!handles || !handle) {
		return LIBUSB_BADARG;
	}

	hdev = usbi_find_dev_handle(handles[0]->dev);
	if (!hdev) {
		return LIBUSB_BADARG;
	}

	ph = hdev->lib_hdl;

	if(!ph) {
		return LIBUSB_BADARG;
	}

	pthread_mutex_lock(&ph->complete_lock);
	list_for_each_entry(io,&ph->complete_list,list) {
	/* safe */
		if(io) {
			/*get the first io on the complete list */
			usbi_debug(hdev->lib_hdl, 4, "complete list: %p\n",
				io->req);

			for(i=0;i<num_reqs;i++) {
			usbi_debug(hdev->lib_hdl, 4, "polling %p",handles[i]);
				if(io->req == handles[i]) {
					ph->complete_count--;
					found = 1;
					break;
				}
			}
			if(found == 1)
				break;
		}
	}

	if(found == 1) { /* find one on the complete list */
		list_del(&io->list); /* remove it from the list */

		*handle = io->req;

		usbi_debug(hdev->lib_hdl, 4, "One was completed: %p",io->req);

		usbi_free_io(io);

	} else { /* Maybe the submitted io not complete, waiting it */
		usbi_debug(hdev->lib_hdl, 4, "No one was completed");
		*handle = NULL;
	}

	pthread_mutex_unlock(&ph->complete_lock);

	return 0;
}

#define USBI_MREQ_NO_NEW_BUF 0
#define USBI_MREQ_NEW_BUF 1
#define USBI_MREQ_STOPPED 2
struct usbi_multi_request {

	/* all mutli-requests are put on the device's mreq list */
	struct list_head list; 
	struct list_head req_head; /* this multi-request's outstanding reqs */

	libusb_multi_request_handle_t mreq; /* user request */

	pthread_mutex_t lock; /* protect this struct */
	pthread_cond_t cv;

	int flag; /* if new buffer added, stopped */
};

struct usbi_multi_req_args {
	struct usbi_multi_request *mi_req;
	uint32_t idx;
	libusb_request_handle_t req;
	struct list_head list;
};

#define min(a, b) ((a) < (b) ? (a) : (b))

/*
 * callback of libusb_xfer_aio()
 * process individual previously submitted libusb_request_handle
 *
 */
int32_t multi_req_callback(libusb_request_handle_t req)
{
	libusb_multi_request_handle_t mreq;
	struct usbi_multi_request *mi_req;
	uint32_t len=0;
	libusb_request_result_t *result;
	libusb_transfer_type_t type;
	libusb_multi_isoc_request_t *isoc;
	uint32_t idx = 0;
	struct usbi_multi_req_args *args;

	args = (struct usbi_multi_req_args *)req->arg;
	usbi_debug(NULL, 4, "args = %p",args);

	list_del(&args->list);

	mi_req = (struct usbi_multi_request*)args->mi_req;
	if(!mi_req) {
		usbi_debug(NULL, 1, "Invalid multi-request handle");
		return LIBUSB_INVALID_HANDLE;
	}

	idx = args->idx;

	usbi_debug(NULL, 1, "Idx = %d",idx);	

	mreq = mi_req->mreq;
	if(!mreq) {
		usbi_debug(NULL, 1, "Multi-Req NULL");	
		return LIBUSB_INVALID_HANDLE;
	}

	type = mreq->type;

	if (type == USB_TYPE_BULK || type == USB_TYPE_INTERRUPT) {

		len = sizeof(struct libusb_request_result);

	} else if (type == USB_TYPE_ISOCHRONOUS) {
		isoc = mreq->req.isoc;

		len = sizeof(struct libusb_request_result) * 
			isoc->pkts[idx].num_packets;
	}

	result = malloc(len);
	if (!result) {
		return LIBUSB_NO_RESOURCES;
	}

	if (type == USB_TYPE_BULK) {
		memcpy(result, &req->req.bulk->result, len);
	} else if (type == USB_TYPE_INTERRUPT) {
		memcpy(result, &req->req.intr->result,len);
	} else if (type == USB_TYPE_ISOCHRONOUS) {
		memcpy(result, req->req.isoc->isoc_results,len);
	}

	free(req);
	free(args);
	
	if(mreq->cb) {
		return(mreq->cb(mreq, idx, result));
	}

	return 0;
}

/*
 * multi xfer request processing thread 
 * split one multi-request to num_bufs libusb_request_handle
 * usbi_multi_request --> usbi_multi_req_args
 *                    --> usbi_multi_req_args
 *		      ...
 *                    --> usbi_multi_req_args
 */
int process_multi_request(void *arg)
{
	struct usbi_multi_request *mi_req = (struct usbi_multi_request *)arg;
	int i;
	libusb_dev_handle_t dev;
	uint8_t ifc;
	uint8_t endpoint;
	libusb_transfer_type_t type;
	libusb_request_handle_t req;
	struct usbi_dev_handle *hdev;
	libusb_multi_request_handle_t mh;
	struct usbi_multi_req_args *args;
	uint32_t req_num = 0;
	int ret;

	usbi_debug(NULL, 4, "Begin");

	if(!mi_req) {
		return LIBUSB_BADARG;
	}

	mh = mi_req->mreq;
	dev = mh->dev;
	ifc = mh->interface;
	endpoint = mh->endpoint;
	type = mh->type;

	hdev = usbi_find_dev_handle(dev);
	if(!hdev) {
		return LIBUSB_BADARG;
	}

loop:
	pthread_mutex_lock(&mi_req->lock);

	if (type == USB_TYPE_BULK) {
		req_num = mh->req.bulk->num_bufs;

	} else if (type == USB_TYPE_INTERRUPT) {

		req_num = mh->req.intr->num_bufs;
		
	} else if (type == USB_TYPE_ISOCHRONOUS) {
	
		req_num = mh->req.isoc->num_pkts;
	}

	usbi_debug(hdev->lib_hdl, 4, "Num_req = %d",req_num);

	for(i = 0; i< req_num; i++) {
		usbi_debug(hdev->lib_hdl, 4, "submit request %d",i);

		req = malloc(sizeof(struct libusb_request_handle));
		if (!req) {
			usbi_debug(hdev->lib_hdl, 1, "No resources");
			return LIBUSB_NO_RESOURCES;
		}

		/* args is the additional argument of callback 
		 * in libusb_request_handle_t
		 */
		args = malloc(sizeof(struct usbi_multi_req_args));
		if (!args) {
			usbi_debug(hdev->lib_hdl, 1, "No resources");
			return LIBUSB_NO_RESOURCES;
		}

		memset(req, 0, sizeof(struct libusb_request_handle));
		memset(args, 0, sizeof(struct usbi_multi_req_args));

		req->dev = mh->dev;
		req->interface = mh->interface;
		req->endpoint = mh->endpoint;
		req->type = mh->type;

		args->mi_req = mi_req;
		usbi_debug(hdev->lib_hdl, 4, "args->mi_req = %p",
			args->mi_req);
		args->idx = i;
		args->req = req;
		
		/* add to this multi-request's ASYNC req list */
		list_add(&args->list, &mi_req->req_head);
		req->arg = args;

		req->cb = multi_req_callback;

		if (type==USB_TYPE_BULK) {
			libusb_multi_bulk_request_t *m_bulk;
			libusb_bulk_request_t *bulk;

			m_bulk = mh->req.bulk;

			/* submit all the request buffers */
			bulk = malloc(sizeof(*bulk));
			if (!bulk) {
				return LIBUSB_NO_RESOURCES;
			}

			memset(bulk, 0, sizeof(struct libusb_bulk_request));

			bulk->payload = m_bulk->payloads[i];
			bulk->length = m_bulk->lengths[i];
			bulk->timeout = m_bulk->timeout;
			bulk->flags = m_bulk->flags;

			req->req.bulk = bulk;

			libusb_xfer_aio(req);

			m_bulk->rp++; /* move rp forward */

		} else if (type == USB_TYPE_INTERRUPT) {
			libusb_multi_intr_request_t *m_intr;
			libusb_intr_request_t *intr;

			m_intr = mh->req.intr;

			intr = malloc(sizeof(*intr));
			if (!intr) {
				return LIBUSB_NO_RESOURCES;
			}

			intr->payload = m_intr->payloads[i];

			intr->length = m_intr->lengths[i];
			intr->timeout = m_intr->timeout;
			intr->flags = m_intr->flags;

			usbi_debug(hdev->lib_hdl, 4, "Intr len=%d,buf=%p",
				intr->length, intr->payload);

			req->req.intr = intr;

			ret = libusb_xfer_aio(req);
			if (ret != 0) {
				usbi_debug(hdev->lib_hdl, 1, "intr aio fail");
				return ret;
			}

			m_intr->rp++; /* move rp forward */

		} else if (type==USB_TYPE_ISOCHRONOUS) {
			libusb_multi_isoc_request_t *m_isoc;
			libusb_isoc_request_t *isoc;

			m_isoc = mh->req.isoc;

			/* submit all the request buffers */
			isoc = malloc(sizeof(*isoc));
			if (!isoc) {
				free(req);
				return LIBUSB_NO_RESOURCES;
			}

			memset(isoc, 0, sizeof(struct libusb_isoc_request));
			isoc->pkts = m_isoc->pkts[i];
			isoc->start_frame = m_isoc->start_frame;
			isoc->flags = m_isoc->flags;

			req->req.isoc = isoc;

			libusb_xfer_aio(req);

			m_isoc->rp++; /* move rp forward */

		} else {
			return(LIBUSB_BADARG);
		}
	}

	mi_req->flag = USBI_MREQ_NO_NEW_BUF; /* clear flag */
	pthread_mutex_unlock(&mi_req->lock);

	pthread_mutex_lock(&mi_req->lock);
	while(mi_req->flag == USBI_MREQ_NO_NEW_BUF) {
		pthread_cond_wait(&mi_req->cv,&mi_req->lock);
	}

	/* this request is stopped. free its resouces and exit this thread */
	if(mi_req->flag == USBI_MREQ_STOPPED) {
		struct usbi_multi_req_args *pargs, *tmp;

		list_for_each_entry_safe(pargs, tmp, &mi_req->req_head, list) {
			libusb_abort(pargs->req);
			free(pargs->req); /*FIXME: should be here ??? */
			free(pargs);
		}

		pthread_mutex_unlock(&mi_req->lock);
		free(mi_req);

		pthread_exit(0);
	}

	pthread_mutex_unlock(&mi_req->lock);
	goto loop;
}

/*
 * turn a libusb_multi_request_handle to internal structure 
 * libusb_multi_request_handle <--> usbi_multi_request
 */
int32_t libusb_start(libusb_multi_request_handle_t handle)
{
	int ret = 0;
	libusb_dev_handle_t dev;
	uint8_t ifc;
	uint8_t endpoint;
	libusb_transfer_type_t type;
	libusb_request_handle_t req;
	struct usbi_dev_handle *hdev;
	struct usbi_multi_request *mi_req;
	pthread_t thread;


	if (!handle) {
		return LIBUSB_BADARG;
	}
	
	dev = handle->dev;
	ifc = handle->interface;
	endpoint = handle->endpoint;
	type = handle->type;

	hdev = usbi_find_dev_handle(dev);
	if(!hdev) {
		usbi_debug(NULL, 1, "invalid device");
		return LIBUSB_BADARG;
	}

	mi_req = malloc(sizeof(struct usbi_multi_request));
	if (!mi_req) {
		usbi_debug(hdev->lib_hdl, 1, "malloc fail");
		return LIBUSB_NO_RESOURCES;
	}

	req = malloc(sizeof(struct libusb_request_handle));
	if (!req) {
		return LIBUSB_NO_RESOURCES;
	}
	memset(req, 0, sizeof(struct libusb_request_handle));

	req->dev = handle->dev;
	req->interface = handle->interface;
	req->endpoint = handle->endpoint;
	req->type = handle->type;

	if(check_req_valid(req, hdev) != 0) {
		free(mi_req);
		ret = -1;
	}

	free(req);
	
	/* submit all the request buffers in the a background thread */
	mi_req->mreq = handle;
	pthread_mutex_init(&mi_req->lock,NULL);
	pthread_cond_init(&mi_req->cv, NULL);
	list_init(&mi_req->list);
	list_init(&mi_req->req_head);

	pthread_create(&thread, NULL,(void*) process_multi_request, mi_req);

	pthread_mutex_lock(&hdev->lock);
	list_add(&mi_req->list, &hdev->m_head);
	pthread_mutex_unlock(&hdev->lock);
	
	usbi_debug(hdev->lib_hdl, 4, "End");
	return ret;
}

int32_t usbi_add_or_stop(libusb_multi_request_handle_t handle, int flag)
{
	struct usbi_multi_request *mreq;
	struct usbi_dev_handle *hdev;

	if (!handle) {
		return LIBUSB_BADARG;
	}

	hdev = usbi_find_dev_handle(handle->dev);
	if(!hdev) {
		return LIBUSB_BADARG;
	}

	pthread_mutex_lock(&hdev->lock);
	mreq = NULL;
	list_for_each_entry(mreq, &hdev->m_head, list) {
	/* safe */
		if(mreq->mreq == handle) {
			break;
		}
	}
	pthread_mutex_unlock(&hdev->lock);

	if (!mreq) {
		/* must call libusb_start first */
		return LIBUSB_INVALID_HANDLE;
	}

	pthread_mutex_lock(&mreq->lock);
	pthread_cond_signal(&mreq->cv);
	mreq->flag = flag;
	pthread_mutex_unlock(&mreq->lock);

	return 0;
}

/*
 * add new xfer request to a previous multi-xfer request 
 * Application should set its new xfer buffers 
 * and num_bufs in the multi-request handle
 * NOTE: application should NOT re-use its previous buffers,unless it's sure
 *    the previous buffers are safe to use. All the internal processing will
 *    use these buffers until user's callback is called.
 */
int32_t libusb_add(libusb_multi_request_handle_t handle)
{
	return (usbi_add_or_stop(handle, USBI_MREQ_NEW_BUF));
}

int32_t libusb_stop(libusb_multi_request_handle_t handle)
{
	return (usbi_add_or_stop(handle, USBI_MREQ_STOPPED));
}
