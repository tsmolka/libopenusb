/*
 * Parses descriptors
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 * Copyright 2001-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdio.h>
#include <stdlib.h>	/* malloc, free */
#include <string.h>	/* memset */

#include "usbi.h"

/*
 * Get device descriptors through the Default endpoint
 */
int usbi_get_descriptor(openusb_dev_handle_t dev, unsigned char type,
	unsigned char index, void *buf, unsigned int buflen)
{
	int ret;
	struct openusb_ctrl_request ctrl = {
		.setup.bmRequestType = USB_ENDPOINT_IN,
		.setup.bRequest = USB_REQ_GET_DESCRIPTOR,
		.setup.wValue = (type << 8) + index,
		.setup.wIndex = 0,
		.payload = buf,
		.length = buflen,
		.timeout = 1000,
	};

	if (!buf || buflen == 0) {
		return OPENUSB_BADARG;
	}

	ret = openusb_ctrl_xfer(dev,0,0,&ctrl);
	if (ret < 0 || ctrl.result.status != OPENUSB_SUCCESS) {
		return -1;
	}

	return(ctrl.result.transferred_bytes);
}

/* FIXME: Should we return OPENUSB_NO_RESOURCES on buffer overflow? */
int openusb_parse_data(char *format, uint8_t *source, uint32_t sourcelen,
	void *dest, uint32_t destlen, uint32_t *count)
{
	unsigned char *sp = source, *dp = dest;
	uint16_t w;
	uint32_t d;
	char *cp;

	if (!format || !source || !dest || !count) {
		return OPENUSB_BADARG;
	}
	for (cp = format; *cp; cp++) {
		switch (*cp) {
		case '.':	/* Skip 8-bit byte */
			sp++; sourcelen--;
			break;
		case 'b':   /* 8-bit byte */
			if (sourcelen < 1 || destlen < 1)
				return OPENUSB_NO_RESOURCES;

			*dp++ = *sp++;
			destlen--; sourcelen--;
			break;
		case 'w': /* 16-bit word, convert from little endian to CPU */
			if (sourcelen < 2 || destlen < 2)
				return OPENUSB_NO_RESOURCES;

			w = (sp[1] << 8) | sp[0]; sp += 2;

			/* Align to word boundary */
			dp += ((unsigned long)dp & 1);
			*((uint16_t *)dp) = w; dp += 2;
			destlen -= 2; sourcelen -= 2;
			break;
		case 'd': /* 32-bit dword, convert from little endian to CPU */
			if (sourcelen < 4 || destlen < 4)
				return OPENUSB_NO_RESOURCES;

			d = (sp[3] << 24) | (sp[2] << 16) |
				(sp[1] << 8) | sp[0]; sp += 4;

			/* Align to dword boundary */
			dp += ((unsigned long)dp & 2);
			*((uint32_t *)dp) = d; dp += 4;
			destlen -= 4; sourcelen -= 4;
			break;
			/*
			 * These two characters are undocumented and just a
			 * hack for Linux
			 */
		case 'W':   /* 16-bit word, keep CPU endianess */
			if (sourcelen < 2 || destlen < 2)
			return OPENUSB_NO_RESOURCES;

			/* Align to word boundary */
			dp += ((unsigned long)dp & 1);
			memcpy(dp, sp, 2); sp += 2; dp += 2;
			destlen -= 2; sourcelen -= 2;
			break;
		case 'D':  /* 32-bit dword, keep CPU endianess */
			if (sourcelen < 4 || destlen < 4)
				return OPENUSB_NO_RESOURCES;

			/* Align to dword boundary */
			dp += ((unsigned long)dp & 2);
			memcpy(dp, sp, 4); sp += 4; dp += 4;
			destlen -= 4; sourcelen -= 4;
			break;
		}
	}

	*count = sp - source;
	return OPENUSB_SUCCESS;
}

/*
 * This code looks surprisingly similar to the code I wrote for the Linux
 * kernel. It's not a coincidence :)
 */

struct usb_descriptor_header {
  uint8_t bLength;
  uint8_t bDescriptorType;
};

/* FIXME: Audit all of the increments to make sure we skip descriptors
 * correctly on errors
 */

static int usbi_parse_endpoint(struct usbi_endpoint *ep,
	unsigned char *buf, unsigned int buflen)
{
	struct usb_descriptor_header header;
	int parsed = 0, numskipped = 0;
	uint32_t count;
	char *extra;
	int extra_len;

	if (!buf || buflen <= 0){
		return OPENUSB_PARSE_ERROR;
	}

	usbi_debug(NULL, 4, "parse ep buflen = %d, buf = %p", buflen, buf);

	openusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

	/*
	 * Everything should be fine being passed into here,
	 * but sanity check JIC 
	 */
	if (header.bLength > buflen) {
		usbi_debug(NULL, 1, "ran out of descriptors parsing");
		return -1;
	}

	if (header.bDescriptorType != USB_DESC_TYPE_ENDPOINT) {
		usbi_debug(NULL, 4,
			"unexpected descriptor 0x%X, expecting "
			"endpoint descriptor, type 0x%X",
			header.bDescriptorType,
			USB_DESC_TYPE_ENDPOINT);
		return parsed;
	}

	if (header.bLength < USBI_ENDPOINT_DESC_SIZE) {
		usbi_debug(NULL, 4, "endpoint descriptor too short."
			" only %d bytes long",
			header.bLength);
		return parsed;
	}

	if (header.bLength >= USBI_ENDPOINT_AUDIO_DESC_SIZE)
		openusb_parse_data("bbbbwbbb", buf, buflen, &ep->desc,
				sizeof(ep->desc), &count);
	else if (header.bLength >= USBI_ENDPOINT_DESC_SIZE)
		openusb_parse_data("bbbbwb", buf, buflen, &ep->desc,
				sizeof(ep->desc), &count);
	/* FIXME: Print error if descriptor is wrong size */

	/* FIXME: Maybe report about extra unparsed data
	 * after the descriptor?
	 */

	/* Skip over the just parsed data */
	buf += header.bLength;
	buflen -= header.bLength;
	parsed += header.bLength;

	/* Skip over the rest of the Class Specific or
	 * Vendor Specific descriptors
	 */
	extra = (char *)buf;	
	extra_len = 0;
	while (buflen >= USBI_DESC_HEADER_SIZE) {
		openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

		if (header.bLength < USBI_DESC_HEADER_SIZE) {
			usbi_debug(NULL, 1, "invalid descriptor length of %d",
				header.bLength);
			return -1;
		}

		/* If we find another "proper" descriptor then we're done */
		if (header.bDescriptorType == USB_DESC_TYPE_ENDPOINT ||
			header.bDescriptorType == USB_DESC_TYPE_INTERFACE ||
			header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
			header.bDescriptorType == USB_DESC_TYPE_DEVICE)
			break;

		usbi_debug(NULL, 4, "skipping descriptor 0x%X",
			header.bDescriptorType);

		numskipped++;

		extra_len += header.bLength;

		buf += header.bLength;
		buflen -= header.bLength;
		parsed += header.bLength;

		usbi_debug(NULL, 4, "parse ep buflen = %d, buf = %p",
			buflen, buf);
	}

	usbi_debug(NULL, 4, "extra len= %d",extra_len);

	if (extra_len) {
		ep->extra = calloc(extra_len, 1);
		if (!ep->extra) {
			return -1;
		}

		memcpy(ep->extra, extra, extra_len);
		ep->extralen = extra_len;
	}

	if (numskipped)
		usbi_debug(NULL, 4, "skipped %d class/vendor specific"
			" endpoint descriptors", numskipped);

	return parsed;
}

static int usbi_get_intf_altno(char *buf, unsigned int buflen)
{
	int fno,altno=1;
	int len,hlen;
	char *p;
	p = buf;

	len = hlen = 0;
	fno = p[2];
	while(len < buflen) {
		hlen = p[0];
		p += hlen;
		len += hlen;

		if((p[1] == USB_DESC_TYPE_INTERFACE) && (p[2] == fno)) {
			altno++;
		}
	}

	usbi_debug(NULL, 4, "altno = %d", altno);
	
	return altno;

}

static int usbi_parse_interface(struct usbi_interface *intf,
	unsigned char *buf, unsigned int buflen)
{
	int i, retval, parsed = 0, numskipped;
	struct usb_descriptor_header header;
	uint8_t alt_num;
	struct usbi_altsetting *as = NULL;
	uint32_t count;
	char *extra;
	int extra_len;

	if (!buf || buflen <= 0){
		return OPENUSB_PARSE_ERROR;
	}

	if (buf[1] != USB_DESC_TYPE_INTERFACE) {
	/* not an interface descriptor,just skip it */
		usbi_debug(NULL, 4, "skipped type %d",buf[1]);
		return buflen;
	}

	usbi_debug(NULL, 4, "parse alt buflen = %d, buf = %p", buflen, buf);

	alt_num = usbi_get_intf_altno((char *)buf,buflen);

	intf->altsettings = calloc(sizeof(intf->altsettings[0]) * alt_num, 1);
	if (!intf->altsettings) {
		intf->num_altsettings = 0;
		usbi_debug(NULL, 1, "couldn't allocated memory"
			" for altsettings");

		return -1;
	}

	intf->num_altsettings = 0;

	while (buflen >= USBI_INTERFACE_DESC_SIZE) {

		openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

		as = intf->altsettings + intf->num_altsettings;
		intf->num_altsettings++;

		openusb_parse_data("bbbbbbbbb", buf, buflen, &as->desc,
				sizeof(as->desc), &count);
		
		usbi_debug(NULL, 4, "interface: num = %d, alt = %d, altno=%d",
			as->desc.bInterfaceNumber,
			as->desc.bAlternateSetting,
			intf->num_altsettings);

		/* Skip over the interface */
		buf += header.bLength;
		parsed += header.bLength;
		buflen -= header.bLength;

		numskipped = 0;
		extra = (char *)buf;
		extra_len = 0;
		/* Skip over any interface, class or vendor descriptors */
		while (buflen >= USBI_DESC_HEADER_SIZE) {
			uint8_t type;

			openusb_parse_data("bb", buf, buflen, &header, 
				sizeof(header), &count);

			if (header.bLength < USBI_DESC_HEADER_SIZE) {
				usbi_debug(NULL, 1,
					"invalid descriptor length of %d", 
					header.bLength);
				free(intf->altsettings);
				return -1;
			}
			
			type = header.bDescriptorType;

			/* If we find another "proper" descriptor 
			 * then we're done 
			 */
			if (type == USB_DESC_TYPE_INTERFACE ||
				type == USB_DESC_TYPE_ENDPOINT ||
				type == USB_DESC_TYPE_CONFIG ||
				type == USB_DESC_TYPE_DEVICE)
				break;

			numskipped++;
			usbi_debug(NULL, 4, "Skipped type: %x", type);
			extra_len += header.bLength;

			buf += header.bLength;
			parsed += header.bLength;
			buflen -= header.bLength;
			usbi_debug(NULL, 4,
				"parse alt extra buflen = %d, buf = %p",
				buflen, buf);
		}
		
		if (numskipped)
			usbi_debug(NULL, 4, "skipped %d class/vendor specific"
				" interface descriptors", numskipped);

		if (extra_len != 0) {
			usbi_debug(NULL, 4, "extra_len: %d", extra_len);

			as->extra = calloc(extra_len, 1);
			if (!as->extra) {
				/* free something */
				usbi_debug(NULL, 4, "malloc fail");
				return -1;
			}
			memcpy(as->extra, extra, extra_len);
			as->extralen = extra_len;
		}

		/* Did we hit an unexpected descriptor? */
		openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

		if (buflen >= USBI_DESC_HEADER_SIZE &&
			(header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
			 header.bDescriptorType == USB_DESC_TYPE_DEVICE))
			return parsed;

		if (as->desc.bNumEndpoints > USBI_MAXENDPOINTS) {
			usbi_debug(NULL, 1,
				"too many endpoints, ignoring rest");
			free(intf->altsettings);
			return -1;
		}

		usbi_debug(NULL, 1, "endpoints:%d", as->desc.bNumEndpoints);
		
		as->endpoints = calloc(as->desc.bNumEndpoints *
			sizeof(struct usbi_endpoint), 1);
		if (!as->endpoints) {
			usbi_debug(NULL, 1,
				"couldn't allocated %d bytes for endpoints",
				as->desc.bNumEndpoints * 
				sizeof(struct usb_endpoint_desc));

			for (i = 0; i < intf->num_altsettings; i++) {
				as = intf->altsettings + intf->num_altsettings;
				free(as->endpoints);
			}
			free(intf->altsettings);

			return -1;      
		}
		as->num_endpoints = as->desc.bNumEndpoints;

		for (i = 0; i < as->num_endpoints; i++) {
			openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

			if (header.bLength > buflen) {
				usbi_debug(NULL, 1, 
					"ran out of descriptors parsing");

				for (i = 0; i <= intf->num_altsettings; i++) {
					as = intf->altsettings + 
						intf->num_altsettings;
					free(as->endpoints);
				}
				free(intf->altsettings);

				return -1;
			}

			retval = usbi_parse_endpoint(as->endpoints + i, buf,
					buflen);
			if (retval < 0) {
				usbi_debug(NULL, 1, "parse endpoint error");
				for (i = 0; i <= intf->num_altsettings; i++) {
					as = intf->altsettings +
						intf->num_altsettings;
					free(as->endpoints);
				}
				free(intf->altsettings);

				return retval;
			}

			buf += retval;
			parsed += retval;
			buflen -= retval;
		}

		/* We check to see if it's an alternate to this one */
		openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

		alt_num = buf[3];
		if (buflen < USBI_INTERFACE_DESC_SIZE ||
			header.bDescriptorType != USB_DESC_TYPE_INTERFACE ||
			!alt_num) {
			return parsed;
		}

	}

	return parsed;
}

int usbi_parse_configuration(struct usbi_config *cfg, unsigned char *buf,
	size_t buflen)
{
	struct usb_descriptor_header header;
	int i, retval;
	uint32_t count;
	char *extra;
	int extra_len;
	int numskipped = 0;
	
	if (!buf || buflen <= 0){
		return OPENUSB_PARSE_ERROR;
	}

	openusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

	/* parse configuration descriptor */
	openusb_parse_data("bbwbbbbb", buf, buflen, &cfg->desc,
			sizeof(cfg->desc), &count);

	if (cfg->desc.bNumInterfaces > USBI_MAXINTERFACES) {
		usbi_debug(NULL, 1, "too many interfaces, ignoring rest");
		return -1;
	}

	cfg->interfaces = calloc(cfg->desc.bNumInterfaces *
			sizeof(cfg->interfaces[0]), 1);
	if (!cfg->interfaces) {
		usbi_debug(NULL, 1, "couldn't allocated %d bytes for interfaces",
				cfg->desc.bNumInterfaces *
				sizeof(cfg->interfaces[0]));
		return -1;      
	}

	cfg->num_interfaces = cfg->desc.bNumInterfaces;

	memset(cfg->interfaces, 0,
		cfg->num_interfaces * sizeof(cfg->interfaces[0]));

	buf += header.bLength;
	buflen -= header.bLength;

	usbi_debug(NULL, 4, "parse cfg buflen = %d, buf = %p", buflen, buf);

	/* Skip over the rest of the Class specific or Vendor
	 * specific descriptors
	 */
	extra = (char *)buf;
	extra_len = 0;
	while (buflen >= USBI_DESC_HEADER_SIZE) {
		uint8_t type;

		openusb_parse_data("bb", buf, buflen, &header,
				sizeof(header), &count);

		if (header.bLength > buflen ||
				header.bLength < USBI_DESC_HEADER_SIZE) {
			usbi_debug(NULL, 1,
					"invalid descriptor length of %d",
					header.bLength);
			free(cfg->interfaces);
			return -1;
		}

		type = header.bDescriptorType;
		/* If we find another "proper" descriptor
		 * then we're done
		 */
		if (type == USB_DESC_TYPE_ENDPOINT ||
				type == USB_DESC_TYPE_INTERFACE ||
				type == USB_DESC_TYPE_CONFIG ||
				type == USB_DESC_TYPE_DEVICE)
			break;

		usbi_debug(NULL, 4, "skipping descriptor 0x%X", type);
		numskipped++;
		extra_len += header.bLength;

		buf += header.bLength;
		buflen -= header.bLength;

		usbi_debug(NULL, 4,
				"parse extra cfg buflen = %d, buf = %p",
				buflen, buf);
	}


	if (numskipped)
		usbi_debug(NULL, 4, "skipped %d class/vendor specific"
				" endpoint descriptors", numskipped);

	if (extra_len) {
		cfg->extra = calloc(extra_len, 1);
		if (cfg->extra == NULL) {
			/* free something */

			return OPENUSB_PARSE_ERROR;
		}

		memcpy(cfg->extra, extra, extra_len);
		cfg->extralen = extra_len;
	} else {
		cfg->extra = NULL;
	}

	for (i = 0; (i < cfg->num_interfaces) && (buflen > 0); i++) {
		retval = usbi_parse_interface(cfg->interfaces + i, buf, buflen);
		if (retval < 0) {
			usbi_debug(NULL, 4, "parse_interface fail");
			free(cfg->interfaces);
			return retval;
		}

		buf += retval;
		buflen -= retval;

	}
	
	return buflen;
}

void usbi_destroy_configuration(struct usbi_device *dev)
{
	int c, i, j, k;
	
	if (!dev->desc.configs) {
		return;
	}

	usbi_debug(NULL, 4, "free %d configs", dev->desc.num_configs);

	for (c = 0; c < dev->desc.num_configs; c++) { /*free config */
		struct usbi_config *cfg = dev->desc.configs + c;

		if (cfg->extra) {
			free(cfg->extra);
		}

		
		if (dev->desc.configs_raw[c].data)
			free(dev->desc.configs_raw[c].data);

		if (!cfg->interfaces) {
			continue;
		}


		for (i = 0; i < cfg->num_interfaces; i++) { /* free intf */
			struct usbi_interface *intf = cfg->interfaces + i;


			for (j = 0; j < intf->num_altsettings; j++) { /* free alt */
				struct usbi_altsetting *as =
					intf->altsettings + j;
				int num_ep = as->num_endpoints;


				for(k = 0; k < num_ep; k++) { /* free ep */
					if (as->endpoints[k].extra) {
						free(as->endpoints[k].extra);
					}
				}

				free(as->endpoints);


				if (as->extra) {
					free(as->extra);
				}

			} /* free alt end */

			if (intf->altsettings)
				free(intf->altsettings);

		} /* free intf end */

		if (cfg->interfaces)
			free(cfg->interfaces);
	} /* free config end */
	
	free(dev->desc.configs_raw);
	free(dev->desc.configs);

	if (dev->desc.device_raw.data)
		free(dev->desc.device_raw.data);

	dev->desc.configs_raw = NULL;
	dev->desc.configs = NULL;
	dev->desc.device_raw.data = NULL;
}

int usbi_get_raw_desc(struct usbi_device *idev, uint8_t type, uint8_t descidx,
        uint16_t langid, uint8_t **buffer, uint16_t *buflen)
{
	if(idev->ops->get_raw_desc) {
		 return idev->ops->get_raw_desc(idev, type, descidx, langid,
		                                 buffer, buflen);
	} else {
		return -1;
	}
}

int usbi_fetch_and_parse_descriptors(struct usbi_dev_handle *hdev)
{
	struct usbi_device *dev = hdev->idev;
	int i;
	int ret;
	char devbuf[USBI_DEVICE_DESC_SIZE+1];
	uint32_t count;

	usbi_destroy_configuration(dev); /* free old descriptors */

	ret = usbi_get_descriptor(hdev->handle, USB_DESC_TYPE_DEVICE,
		0, devbuf, USBI_DEVICE_DESC_SIZE);

	if (ret < 0) {
		usbi_debug(NULL, 2, "Fail to get device descriptors: %d", ret);

		return (OPENUSB_PARSE_ERROR);
	}

	ret = openusb_parse_data("bbwbbbbwwwbbbb", (uint8_t *)devbuf,
		USBI_DEVICE_DESC_SIZE, &dev->desc.device,
		sizeof(dev->desc.device), &count);

	if (ret < 0 || count < USBI_DEVICE_DESC_SIZE) {
		usbi_debug(NULL, 4, "fail to parse device descr");
		return OPENUSB_PARSE_ERROR;
	}

	dev->desc.device_raw.data = calloc(count, 1);
	memcpy(dev->desc.device_raw.data, devbuf, count);

	dev->desc.num_configs = dev->desc.device.bNumConfigurations;

	if (dev->desc.num_configs > USBI_MAXCONFIG) {
		usbi_debug(NULL, 1, "too many configurations (%d > %d)", 
			dev->desc.num_configs, USBI_MAXCONFIG);
		goto err;
	}

	if (dev->desc.num_configs < 1) {
		usbi_debug(NULL, 1, "not enough configurations (%d < 1)",
				dev->desc.num_configs);
		goto err;
	}

	dev->desc.configs_raw = calloc(dev->desc.num_configs *
			sizeof(dev->desc.configs_raw[0]), 1);
	if (!dev->desc.configs_raw) {
		usbi_debug(NULL, 1,
			"unable to allocate %d bytes for cached descriptors",
			dev->desc.num_configs *
			sizeof(dev->desc.configs_raw[0]));
		goto err;
	}

	dev->desc.configs = calloc(dev->desc.num_configs * 
			sizeof(struct usbi_config), 1);
	if (!dev->desc.configs) {
		usbi_debug(NULL, 1,
			"unable to allocate memory for config descriptors",
			dev->desc.num_configs * sizeof(dev->desc.configs[0]));
		goto err;
	}

	for (i = 0; i < dev->desc.num_configs; i++) {
		unsigned char buf[8];
		struct usb_config_desc cfg_desc;
		struct usbi_raw_desc *cfgr = dev->desc.configs_raw + i;

		/*
		 * Get the first 8 bytes so we can figure out
		 * what the total length is
		 */
		ret = usbi_get_descriptor(hdev->handle, USB_DESC_TYPE_CONFIG,
			i, buf, 8);
		if (ret < 8) {
			if (ret < 0)
				usbi_debug(NULL, 1,
					"unable to get first 8 bytes of config"
						" descriptor (ret = %d)", ret);
			else
				usbi_debug(NULL,1,
					"config descriptor too short"
					" (expected 8, got %d)",
						ret);
			goto err;
		}

		openusb_parse_data("bbw", buf, 8, &cfg_desc,
			sizeof(cfg_desc), &count);

		cfgr->len = cfg_desc.wTotalLength;

		cfgr->data = calloc(cfgr->len, 1);
		if (!cfgr->data) {
			usbi_debug(NULL, 1,
				"unable to allocate %d bytes for descriptors",
					cfgr->len);
			goto err;
		}

		ret = usbi_get_descriptor(hdev->handle, USB_DESC_TYPE_CONFIG, i,
				cfgr->data, cfgr->len);
		if (ret < cfgr->len) {
			if (ret < 0)
				usbi_debug(NULL, 1,
				    "unable to get rest of config descriptor"
				    " (ret = %d)", ret);
			else
				usbi_debug(NULL, 1,
				    "config descriptor too short (expected"
				    " %d, got %d)", cfgr->len, ret);

			cfgr->len = 0;
			free(cfgr->data);
			goto err;
		}

		ret = usbi_parse_configuration(dev->desc.configs + i,
			cfgr->data, cfgr->len);
		if (ret > 0)
			usbi_debug(NULL, 2,
				"%d bytes of descriptor data still left", ret);
		else if (ret < 0)
			usbi_debug(NULL, 2, "unable to parse descriptors");
	}

	return 0;

err:
	/* FIXME: Free already allocated config descriptors too */
	free(dev->desc.configs);
	free(dev->desc.configs_raw);

	dev->desc.configs = NULL;
	dev->desc.configs_raw = NULL;

	dev->desc.num_configs = 0;

	return -1;
}
