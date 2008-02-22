/*
 * descriptors structures
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 * Copyright 2001-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#ifndef _DESCR_H
#define _DESCR_H

#include "openusb.h"

/* descriptor structures, to be removed ? */
/* Sizes of various common descriptors */
#define USBI_DESC_HEADER_SIZE		2
#define USBI_DEVICE_DESC_SIZE		18
#define USBI_CONFIG_DESC_SIZE		9
#define USBI_INTERFACE_DESC_SIZE	9
#define USBI_ENDPOINT_DESC_SIZE		7
#define USBI_ENDPOINT_AUDIO_DESC_SIZE	9

struct usbi_endpoint {
  struct usb_endpoint_desc desc;

  char *extra;
  uint16_t extralen;
};

#define USBI_MAXENDPOINTS		32
struct usbi_altsetting {
  struct usb_interface_desc desc;

  size_t num_endpoints;
  struct usbi_endpoint *endpoints;

  char *extra;
  size_t extralen;
};

#define USBI_MAXALTSETTING		128	/* Hard limit */
struct usbi_interface {
  size_t num_altsettings;
  struct usbi_altsetting *altsettings;
};

#define USBI_MAXINTERFACES		32
struct usbi_config {
  usb_config_desc_t desc;

  size_t num_interfaces;
  struct usbi_interface *interfaces;

  char *extra;
  size_t extralen;
};

struct usbi_raw_desc {
  unsigned char *data;
  size_t len;
};

#define USBI_MAXCONFIG			8
struct usbi_descriptors {
  struct usbi_raw_desc device_raw;

  size_t num_configs;
  struct usbi_raw_desc *configs_raw;

  usb_device_desc_t	device;
  struct usbi_config *configs;
};

#endif
