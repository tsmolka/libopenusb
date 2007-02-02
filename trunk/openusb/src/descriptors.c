/*
 * Parses descriptors
 *
 * Copyright 2001-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdio.h>
#include <stdlib.h>	/* malloc, free */
#include <string.h>	/* memset */

#include "usbi.h"

int libusb_get_descriptor(libusb_dev_handle_t dev, unsigned char type,
	unsigned char index, void *buf, unsigned int buflen)
{
  struct libusb_ctrl_request ctrl = {
    .dev = dev,
    .endpoint = 0,
    .setup.bRequestType = USB_ENDPOINT_IN,
    .setup.bRequest = USB_REQ_GET_DESCRIPTOR,
    .setup.wValue = (type << 8) + index,
    .setup.wIndex = 0,
    .buf = buf,
    .buflen = buflen,
    .timeout = 1000,
  };
  size_t transferred_bytes;

  return libusb_ctrl(&ctrl, &transferred_bytes);
}

/* FIXME: Should we return LIBUSB_NO_RESOURCES on buffer overflow? */
int libusb_parse_data(char *format, unsigned char *source, size_t sourcelen,
	void *dest, size_t destlen, size_t *count)
{
  unsigned char *sp = source, *dp = dest;
  uint16_t w;
  uint32_t d;
  char *cp;

  for (cp = format; *cp; cp++) {
    switch (*cp) {
    case '.':	/* Skip 8-bit byte */
      sp++; sourcelen--;
      break;
    case 'b':   /* 8-bit byte */
      if (sourcelen < 1 || destlen < 1)
        return LIBUSB_NO_RESOURCES;

      *dp++ = *sp++;
      destlen--; sourcelen--;
      break;
    case 'w':   /* 16-bit word, convert from little endian to CPU */
      if (sourcelen < 2 || destlen < 2)
        return LIBUSB_NO_RESOURCES;

      w = (sp[1] << 8) | sp[0]; sp += 2;
      dp += ((unsigned long)dp & 1);    /* Align to word boundary */
      *((uint16_t *)dp) = w; dp += 2;
      destlen -= 2; sourcelen -= 2;
      break;
    case 'd':   /* 32-bit dword, convert from little endian to CPU */
      if (sourcelen < 4 || destlen < 4)
        return LIBUSB_NO_RESOURCES;

      d = (sp[3] << 24) | (sp[2] << 16) | (sp[1] << 8) | sp[0]; sp += 4;
      dp += ((unsigned long)dp & 2);    /* Align to dword boundary */
      *((uint32_t *)dp) = d; dp += 4;
      destlen -= 4; sourcelen -= 4;
      break;
    /* These two characters are undocumented and just a hack for Linux */
    case 'W':   /* 16-bit word, keep CPU endianess */
      if (sourcelen < 2 || destlen < 2)
        return LIBUSB_NO_RESOURCES;

      dp += ((unsigned long)dp & 1);    /* Align to word boundary */
      memcpy(dp, sp, 2); sp += 2; dp += 2;
      destlen -= 2; sourcelen -= 2;
      break;
    case 'D':   /* 32-bit dword, keep CPU endianess */
      if (sourcelen < 4 || destlen < 4)
        return LIBUSB_NO_RESOURCES;

      dp += ((unsigned long)dp & 2);    /* Align to dword boundary */
      memcpy(dp, sp, 4); sp += 4; dp += 4;
      destlen -= 4; sourcelen -= 4;
      break;
    }
  }

  *count = sp - source;
  return LIBUSB_SUCCESS;
}

/*
 * This code looks surprisingly similar to the code I wrote for the Linux
 * kernel. It's not a coincidence :)
 */

struct usb_descriptor_header {
  uint8_t bLength;
  uint8_t bDescriptorType;
};

/* FIXME: Audit all of the increments to make sure we skip descriptors correctly on errors */

static int usbi_parse_endpoint(struct usbi_endpoint *ep,
	unsigned char *buf, unsigned int buflen)
{
  struct usb_descriptor_header header;
  int parsed = 0, numskipped = 0;
  size_t count;

  libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

  /* Everything should be fine being passed into here, but sanity check JIC */
  if (header.bLength > buflen) {
    usbi_debug(1, "ran out of descriptors parsing");
    return -1;
  }
                
  if (header.bDescriptorType != USB_DESC_TYPE_ENDPOINT) {
    usbi_debug(2, "unexpected descriptor 0x%X, expecting endpoint descriptor, type 0x%X",
	header.bDescriptorType, USB_DESC_TYPE_ENDPOINT);
    return parsed;
  }

  if (header.bLength < USBI_ENDPOINT_DESC_SIZE) {
    usbi_debug(2, "endpoint descriptor too short. only %d bytes long",
	header.bLength);
    return parsed;
  }

  if (header.bLength >= USBI_ENDPOINT_AUDIO_DESC_SIZE)
    libusb_parse_data("..bbwbbb", buf, buflen, &ep->desc, sizeof(ep->desc), &count);
  else if (header.bLength >= USBI_ENDPOINT_DESC_SIZE)
    libusb_parse_data("..bbwb", buf, buflen, &ep->desc, sizeof(ep->desc), &count);
  /* FIXME: Print error if descriptor is wrong size */

  /* FIXME: Maybe report about extra unparsed data after the descriptor? */

  /* Skip over the just parsed data */
  buf += header.bLength;
  buflen -= header.bLength;
  parsed += header.bLength;

  /* Skip over the rest of the Class Specific or Vendor Specific descriptors */
  while (buflen >= USBI_DESC_HEADER_SIZE) {
    libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

    if (header.bLength < USBI_DESC_HEADER_SIZE) {
      usbi_debug(1, "invalid descriptor length of %d", header.bLength);
      return -1;
    }

    /* If we find another "proper" descriptor then we're done  */
    if (header.bDescriptorType == USB_DESC_TYPE_ENDPOINT ||
        header.bDescriptorType == USB_DESC_TYPE_INTERFACE ||
        header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
        header.bDescriptorType == USB_DESC_TYPE_DEVICE)
      break;

    usbi_debug(1, "skipping descriptor 0x%X", header.bDescriptorType);

    numskipped++;

    buf += header.bLength;
    buflen -= header.bLength;
    parsed += header.bLength;
  }

  if (numskipped)
    usbi_debug(2, "skipped %d class/vendor specific endpoint descriptors",
	numskipped);

  return parsed;
}

static int usbi_parse_interface(struct usbi_interface *intf,
	unsigned char *buf, unsigned int buflen)
{
  int i, retval, parsed = 0, numskipped;
  struct usb_descriptor_header header;
  uint8_t alt_num;
  struct usbi_altsetting *as;
  size_t count;

  intf->num_altsettings = 0;

  while (buflen >= USBI_INTERFACE_DESC_SIZE) {
    libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

    intf->altsettings = realloc(intf->altsettings, sizeof(intf->altsettings[0]) * (intf->num_altsettings + 1));
    if (!intf->altsettings) {
      intf->num_altsettings = 0;
      usbi_debug(1, "couldn't allocated %d bytes for altsettings",
	sizeof(intf->altsettings[0]) * (intf->num_altsettings + 1));

      return -1;
    }

    as = intf->altsettings + intf->num_altsettings;
    intf->num_altsettings++;

    libusb_parse_data("..bbbbbbb", buf, buflen, &as->desc, sizeof(as->desc), &count);

    /* Skip over the interface */
    buf += header.bLength;
    parsed += header.bLength;
    buflen -= header.bLength;

    numskipped = 0;

    /* Skip over any interface, class or vendor descriptors */
    while (buflen >= USBI_DESC_HEADER_SIZE) {
      libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

      if (header.bLength < USBI_DESC_HEADER_SIZE) {
        usbi_debug(1, "invalid descriptor length of %d", header.bLength);
        free(intf->altsettings);
        return -1;
      }

      /* If we find another "proper" descriptor then we're done */
      if (header.bDescriptorType == USB_DESC_TYPE_INTERFACE ||
          header.bDescriptorType == USB_DESC_TYPE_ENDPOINT ||
          header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
          header.bDescriptorType == USB_DESC_TYPE_DEVICE)
        break;

      numskipped++;

      buf += header.bLength;
      parsed += header.bLength;
      buflen -= header.bLength;
    }

    if (numskipped)
      usbi_debug(2, "skipped %d class/vendor specific interface descriptors",
	numskipped);

    /* Did we hit an unexpected descriptor? */
    libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

    if (buflen >= USBI_DESC_HEADER_SIZE &&
        (header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
         header.bDescriptorType == USB_DESC_TYPE_DEVICE))
      return parsed;

    if (as->desc.bNumEndpoints > USBI_MAXENDPOINTS) {
      usbi_debug(1, "too many endpoints, ignoring rest");
      free(intf->altsettings);
      return -1;
    }

    as->endpoints = malloc(as->desc.bNumEndpoints *
                     sizeof(struct usb_endpoint_desc));
    if (!as->endpoints) {
      usbi_debug(1, "couldn't allocated %d bytes for endpoints",
	as->desc.bNumEndpoints * sizeof(struct usb_endpoint_desc));

      for (i = 0; i < intf->num_altsettings; i++) {
        as = intf->altsettings + intf->num_altsettings;
        free(as->endpoints);
      }
      free(intf->altsettings);

      return -1;      
    }
    as->num_endpoints = as->desc.bNumEndpoints;

    memset(as->endpoints, 0, as->num_endpoints * sizeof(as->endpoints[0]));

    for (i = 0; i < as->num_endpoints; i++) {
      libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

      if (header.bLength > buflen) {
        usbi_debug(1, "ran out of descriptors parsing");

        for (i = 0; i <= intf->num_altsettings; i++) {
          as = intf->altsettings + intf->num_altsettings;
          free(as->endpoints);
        }
        free(intf->altsettings);

        return -1;
      }
                
      retval = usbi_parse_endpoint(as->endpoints + i, buf, buflen);
      if (retval < 0) {
        for (i = 0; i <= intf->num_altsettings; i++) {
          as = intf->altsettings + intf->num_altsettings;
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
    libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

    alt_num = buf[3];
    if (buflen < USBI_INTERFACE_DESC_SIZE ||
        header.bDescriptorType != USB_DESC_TYPE_INTERFACE ||
        !alt_num)
      return parsed;
  }

  return parsed;
}

int usbi_parse_configuration(struct usbi_config *cfg, unsigned char *buf,
	size_t buflen)
{
  struct usb_descriptor_header header;
  int i, retval;
  size_t count;

  libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

  libusb_parse_data("..wbbbbb", buf, buflen, &cfg->desc, sizeof(cfg->desc), &count);

  if (cfg->desc.bNumInterfaces > USBI_MAXINTERFACES) {
    usbi_debug(1, "too many interfaces, ignoring rest");
    return -1;
  }

  cfg->interfaces = malloc(cfg->desc.bNumInterfaces * sizeof(cfg->interfaces[0]));
  if (!cfg->interfaces) {
    usbi_debug(1, "couldn't allocated %d bytes for interfaces",
	cfg->desc.bNumInterfaces * sizeof(cfg->interfaces[0]));
    return -1;      
  }

  cfg->num_interfaces = cfg->desc.bNumInterfaces;

  memset(cfg->interfaces, 0, cfg->num_interfaces * sizeof(cfg->interfaces[0]));

  buf += header.bLength;
  buflen -= header.bLength;
        
  for (i = 0; i < cfg->num_interfaces; i++) {
    int numskipped = 0;

    /* Skip over the rest of the Class specific or Vendor specific descriptors */
    while (buflen >= USBI_DESC_HEADER_SIZE) {
      libusb_parse_data("bb", buf, buflen, &header, sizeof(header), &count);

      if (header.bLength > buflen || header.bLength < USBI_DESC_HEADER_SIZE) {
        usbi_debug(1, "invalid descriptor length of %d", header.bLength);
	free(cfg->interfaces);
        return -1;
      }

      /* If we find another "proper" descriptor then we're done */
      if (header.bDescriptorType == USB_DESC_TYPE_ENDPOINT ||
          header.bDescriptorType == USB_DESC_TYPE_INTERFACE ||
          header.bDescriptorType == USB_DESC_TYPE_CONFIG ||
          header.bDescriptorType == USB_DESC_TYPE_DEVICE)
        break;

      usbi_debug(2, "skipping descriptor 0x%X", header.bDescriptorType);
      numskipped++;

      buf += header.bLength;
      buflen -= header.bLength;
    }

    if (numskipped)
      usbi_debug(2, "skipped %d class/vendor specific endpoint descriptors\n",
	numskipped);

    retval = usbi_parse_interface(cfg->interfaces + i, buf, buflen);
    if (retval < 0) {
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
  int c, i, j;
        
  if (dev->desc.configs) {
    for (c = 0; c < dev->desc.num_configs; c++) {
      struct usbi_config *cfg = dev->desc.configs + c;

      if (cfg->interfaces) {
        for (i = 0; i < cfg->num_interfaces; i++) {
          struct usbi_interface *intf = cfg->interfaces + i;
                                
          for (j = 0; j < intf->num_altsettings; j++) {
            struct usbi_altsetting *as = intf->altsettings + j;
                                        
            free(as->endpoints);
          }

          free(intf->altsettings);
        }

        free(cfg->interfaces);
      }

      free(dev->desc.configs_raw[c].data);
    }

    free(dev->desc.configs_raw);
    free(dev->desc.configs);
  }

  if (dev->desc.device_raw.data)
    free(dev->desc.device_raw.data);
}

int usbi_fetch_and_parse_descriptors(struct usbi_dev_handle *hdev)
{
  struct usbi_device *dev = hdev->idev;
  int i;

  dev->desc.num_configs = dev->desc.device.bNumConfigurations;

  if (dev->desc.num_configs > USBI_MAXCONFIG) {
    usbi_debug(1, "too many configurations (%d > %d)", 
	dev->desc.num_configs, USBI_MAXCONFIG);
    goto err;
  }

  if (dev->desc.num_configs < 1) {
    usbi_debug(1, "not enough configurations (%d < 1)",
	dev->desc.num_configs);
    goto err;
  }

  dev->desc.configs_raw = malloc(dev->desc.num_configs * sizeof(dev->desc.configs_raw[0]));
  if (!dev->desc.configs_raw) {
    usbi_debug(1, "unable to allocate %d bytes for cached descriptors",
	dev->desc.num_configs * sizeof(dev->desc.configs_raw[0]));
    goto err;
  }

  memset(dev->desc.configs_raw, 0, dev->desc.num_configs * sizeof(dev->desc.configs_raw[0]));

  dev->desc.configs = malloc(dev->desc.num_configs * sizeof(dev->desc.configs[0]));
  if (!dev->desc.configs) {
    usbi_debug(1, "unable to allocate memory for config descriptors",
	dev->desc.num_configs * sizeof(dev->desc.configs[0]));
    goto err;
  }

  memset(dev->desc.configs, 0, dev->desc.num_configs * sizeof(dev->desc.configs[0]));

  for (i = 0; i < dev->desc.num_configs; i++) {
    unsigned char buf[8];
    struct usb_config_desc cfg_desc;
    struct usbi_raw_desc *cfgr = dev->desc.configs_raw + i;
    size_t count;
    int ret;

    /* Get the first 8 bytes so we can figure out what the total length is */
    ret = libusb_get_descriptor(hdev->handle, USB_DESC_TYPE_CONFIG, i, buf, 8);
    if (ret < 8) {
      if (ret < 0)
        usbi_debug(1, "unable to get first 8 bytes of config descriptor (ret = %d)",
		ret);
      else
        usbi_debug(1, "config descriptor too short (expected 8, got %d)", ret);

      goto err;
    }

    libusb_parse_data("bbw", buf, 8, &cfg_desc, sizeof(cfg_desc), &count);

    cfgr->len = cfg_desc.wTotalLength;

    cfgr->data = malloc(cfgr->len);
    if (!cfgr->data) {
      usbi_debug(1, "unable to allocate %d bytes for descriptors", cfgr->len);
      goto err;
    }

    ret = libusb_get_descriptor(hdev->handle, USB_DESC_TYPE_CONFIG, i, cfgr->data, cfgr->len);
    if (ret < cfgr->len) {
      if (ret < 0)
        usbi_debug(1, "unable to get rest of config descriptor (ret = %d)",
		ret);
      else
        usbi_debug(1, "config descriptor too short (expected %d, got %d)",
		cfgr->len, ret);

      cfgr->len = 0;
      free(cfgr->data);
      goto err;
    }

    ret = usbi_parse_configuration(dev->desc.configs + i, cfgr->data, cfgr->len);
    if (ret > 0)
      usbi_debug(2, "%d bytes of descriptor data still left", ret);
    else if (ret < 0)
      usbi_debug(2, "unable to parse descriptors");
  }

  return 0;

err:
  /* FIXME: Free already allocated config descriptors too */
  free(dev->desc.configs);
  free(dev->desc.configs_raw);

  dev->desc.configs = NULL;
  dev->desc.configs_raw = NULL;

  dev->desc.num_configs = 0;

  return 1;
}

