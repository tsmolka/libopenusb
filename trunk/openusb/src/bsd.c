/*
 * (Free|Open|Net)BSD USB support
 *
 * Derived from Linux version by Richard Tobin.
 *
 * $Id: bsd.c 636 2006-04-07 17:47:13Z jerdfelt $
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

/*
 * Note: I don't have a clue what I'm doing.  I just looked at the
 * man pages and source to try and find things that did the same as
 * the Linux version. -- Richard
 *
 * johnjen@reynoldsnet.org - minor fixes with debug mode output. Consistent brace
 * use as well as indenting. More error messages put in to test for failure
 * modes with /dev/ permissions (when it happens). Note: I, like Richard, have
 * no clue what I'm doing. Patches to increase/fix functionality happily
 * accepted!
 */

/* dirkx@webweaving.org - minor changes to make things actually work
 * 	for both read and write.
 */

/* johannes@erdfelt.com - support both old and new structure naming
 * conventions. Minor cleanups
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include <dev/usb/usb.h>

#include "usbi.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Simplify some of the code below */
#if __FreeBSD__
#define EP_FORMAT	"%d"
#else
#define EP_FORMAT	"%02d"
#endif

/*
 * These structures are used internally to the BSD port. We are free to
 * change these between releases as we see fit.
 */
struct usbi_device_priv {
  char filename[PATH_MAX + 1];
};

#ifdef HAVE_OLD_DEV_USB_USB_H
/*
 * It appears some of the BSD's (OpenBSD atleast) have switched over to a
 * new naming convention, so we setup some macro's for backward
 * compability with older versions --jerdfelt
 */

/* struct usb_ctl_request */
#define ucr_addr		addr
#define ucr_request		request
#define ucr_data		data
#define ucr_flags		flags
#define ucr_actlen		actlen

/* struct usb_alt_interface */
#define uai_config_index	config_index
#define uai_interface_index	interface_index
#define uai_alt_no		alt_no

/* struct usb_config_desc */
#define ucd_config_index	config_index
#define ucd_desc		desc

/* struct usb_interface_desc */
#define uid_config_index	config_index
#define uid_interface_index	interface_index
#define uid_alt_index		alt_index
#define uid_desc		desc

/* struct usb_endpoint_desc */
#define ued_config_index	config_index
#define ued_interface_index	interface_index
#define ued_alt_index		alt_index
#define ued_endpoint_index	endpoint_index
#define ued_desc		desc

/* struct usb_full_desc */
#define ufd_config_index	config_index
#define ufd_size		size
#define ufd_data		data

/* struct usb_string_desc */
#define usd_string_index	string_index
#define usd_language_id		language_id
#define usd_desc		desc

/* struct usb_ctl_report_desc */
#define ucrd_size		size
#define ucrd_data		data

/* struct usb_device_info */
#define udi_bus			bus
#define udi_addr		addr
#define udi_cookie		cookie
#define udi_product		product
#define udi_vendor		vendor
#define udi_release		release
#define udi_productNo		productNo
#define udi_vendorNo		vendorNo
#define udi_releaseNo		releaseNo
#define udi_class		class
#define udi_subclass		subclass
#define udi_protocol		protocol
#define udi_config		config
#define udi_lowspeed		lowspeed
#define udi_power		power
#define udi_nports		nports
#define udi_devnames		devnames
#define udi_ports		ports

/* struct usb_ctl_report */
#define ucr_report		report
#define ucr_data		data

/* struct usb_device_stats */
#define uds_requests		requests
#endif

static int ensure_ep_open(usb_dev_handle *dev, int ep, int mode);

#define MAX_CONTROLLERS 10

/* This records the file descriptors for the endpoints.  It doesn't seem
   to work to re-open them for each read (as well as being inefficient). */

struct bsd_usb_dev_handle_info {
    int ep_fd[USB_MAX_ENDPOINTS];
};

int usb_os_open(usb_dev_handle *dev)
{
  struct usbi_device_priv *dpriv = dev->device->priv;
  int i;
  struct bsd_usb_dev_handle_info *info;
  char ctlpath[PATH_MAX + 1];

  info = malloc(sizeof(struct bsd_usb_dev_handle_info));
  if (!info)
    USB_ERROR(-ENOMEM);
  dev->impl_info = info;

#if __FreeBSD__
  snprintf(ctlpath, PATH_MAX, "%s", dpriv->filename);
#else
  snprintf(ctlpath, PATH_MAX, "%s.00", dpriv->filename);
#endif
  dev->fd = open(ctlpath, O_RDWR);
  if (dev->fd < 0) {
    dev->fd = open(ctlpath, O_RDONLY);
    if (dev->fd < 0) {
      free(info);
      USB_ERROR_STR(-errno, "failed to open %s: %s",
                    ctlpath, strerror(errno));
    }
  }

  /* Mark the endpoints as not yet open */
  for (i = 0; i < USB_MAX_ENDPOINTS; i++)
    info->ep_fd[i] = -1;

  return 0;
}

int usb_os_close(usb_dev_handle *dev)
{
  struct bsd_usb_dev_handle_info *info = dev->impl_info;
  int i;

  /* Close any open endpoints */

  for (i = 0; i < USB_MAX_ENDPOINTS; i++)
    if (info->ep_fd[i] >= 0) {
      if (usb_debug >= 2)
        fprintf(stderr, "usb_os_close: closing endpoint %d\n", info->ep_fd[i]);
      close(info->ep_fd[i]);
    }

  free(info);

  if (dev->fd <= 0)
    return 0;

  if (close(dev->fd) == -1)
    /* Failing trying to close a file really isn't an error, so return 0 */
    USB_ERROR_STR(0, "tried to close device fd %d: %s", dev->fd,
                  strerror(errno));

  return 0;
}

int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
  int ret;

  ret = ioctl(dev->fd, USB_SET_CONFIG, &configuration);
  if (ret < 0)
    USB_ERROR_STR(ret, "could not set config %d: %s", configuration,
                  strerror(errno));

  dev->config = configuration;

  return 0;
}

int usb_claim_interface(usb_dev_handle *dev, int interface)
{
  /* BSD doesn't have the corresponding ioctl.  It seems to
     be sufficient to open the relevant endpoints as needed. */

  dev->interface = interface;

  return 0;
}

int usb_release_interface(usb_dev_handle *dev, int interface)
{
  /* See above */
  return 0;
}

int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
  int ret;
  struct usb_alt_interface intf;

  if (dev->interface < 0)
    USB_ERROR(-EINVAL);

  intf.uai_interface_index = dev->interface;
  intf.uai_alt_no = alternate;

  ret = ioctl(dev->fd, USB_SET_ALTINTERFACE, &intf);
  if (ret < 0)
    USB_ERROR_STR(ret, "could not set alt intf %d/%d: %s",
                  dev->interface, alternate, strerror(errno));

  dev->altsetting = alternate;

  return 0;
}

static int ensure_ep_open(usb_dev_handle *dev, int ep, int mode)
{
  struct usbi_device_priv *dpriv = dev->device->priv;
  struct bsd_usb_dev_handle_info *info = dev->impl_info;
  int fd;
  char buf[20];

  /* Get the address for this endpoint; we could check
   * the mode against the direction; but we've done that
   * already in the usb_bulk_read/write.
   */
  ep = UE_GET_ADDR(ep);

  if (info->ep_fd[ep] < 0) {
    snprintf(buf, sizeof(buf) - 1, "%s." EP_FORMAT, dpriv->filename, ep);

    /* Try to open it O_RDWR first for those devices which have in and out
     * endpoints with the same address (eg 0x02 and 0x82)
     */
    fd = open(buf, O_RDWR);
    if (fd < 0 && errno == ENXIO)
      fd = open(buf, mode);
    if (fd < 0)
      USB_ERROR_STR(fd, "can't open %s for bulk read: %s\n",
                    buf, strerror(errno));
    info->ep_fd[ep] = fd;
  }

  return info->ep_fd[ep];
}

int usb_bulk_write(usb_dev_handle *dev, unsigned char ep, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct usbi_device_priv *dpriv = dev->device->priv;
  int fd, ret, sent = 0;

  /* Ensure the endpoint address is correct */
  ep &= ~USB_ENDPOINT_IN;

  fd = ensure_ep_open(dev, ep, O_WRONLY);
  if (fd < 0) {
    if (usb_debug >= 2)
      fprintf(stderr, "usb_bulk_write: got negative open file descriptor for endpoint " EP_FORMAT "\n", UE_GET_ADDR(ep));

    return fd;
  }

  ret = ioctl(fd, USB_SET_TIMEOUT, &timeout);
  if (ret < 0)
    USB_ERROR_STR(ret, "error setting timeout: %s",
                  strerror(errno));

  do {
    ret = write(fd, bytes + sent, numbytes - sent);
    if (ret < 0)
      USB_ERROR_STR(ret, "error writing to bulk endpoint %s." EP_FORMAT ": %s",
                    dpriv->filename, UE_GET_ADDR(ep), strerror(errno));

    sent += ret;
  } while(ret > 0 && sent < numbytes);

  return sent;
}

int usb_bulk_read(usb_dev_handle *dev, unsigned char ep, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct usbi_device_priv *dpriv = dev->device->priv;
  int fd, ret, retrieved = 0;

  /* Ensure the endpoint address is correct */
  ep |= USB_ENDPOINT_IN;

  fd = ensure_ep_open(dev, ep, O_RDONLY);
  if (fd < 0) {
    if (usb_debug >= 2)
      fprintf(stderr, "usb_bulk_read: got negative open file descriptor for endpoint " EP_FORMAT "\n", UE_GET_ADDR(ep));

    return fd;
  }

  ret = ioctl(fd, USB_SET_TIMEOUT, &timeout);
  if (ret < 0)
    USB_ERROR_STR(ret, "error setting timeout: %s",
                  strerror(errno));

  do {
    ret = read(fd, bytes + retrieved, numbytes - retrieved);
    if (ret < 0)
      USB_ERROR_STR(ret, "error reading from bulk endpoint %s." EP_FORMAT ": %s",
                    dpriv->filename, UE_GET_ADDR(ep), strerror(errno));

    retrieved += ret;
  } while (ret > 0 && retrieved < numbytes);

  return retrieved;
}

int usb_control_msg(usb_dev_handle *dev, uint8_t bRequestType, uint8_t bRequest,
	uint16_t wValue, uint16_t wIndex, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct usb_ctl_request req;
  int ret;

  if (usb_debug >= 3)
    fprintf(stderr, "usb_control_msg: %d %d %d %d %p %d %d\n",
            bRequestType, bRequest, wValue, wIndex, bytes, numbytes, timeout);

  req.ucr_request.bmRequestType = bRequestType;
  req.ucr_request.bRequest = bRequest;
  USETW(req.ucr_request.wValue, wValue);
  USETW(req.ucr_request.wIndex, wIndex);
  USETW(req.ucr_request.wLength, numbytes);

  req.ucr_data = bytes;
  req.ucr_flags = 0;

  ret = ioctl(dev->fd, USB_SET_TIMEOUT, &timeout);
#if (__NetBSD__ || __OpenBSD__)
  if (ret < 0 && errno != EINVAL)
#else
  if (ret < 0)
#endif
    USB_ERROR_STR(ret, "error setting timeout: %s",
                  strerror(errno));

  ret = ioctl(dev->fd, USB_DO_REQUEST, &req);
  if (ret < 0)
    USB_ERROR_STR(ret, "error sending control message: %s",
                  strerror(errno));

  return UGETW(req.ucr_request.wLength);
}

int usb_os_find_busses(struct usb_bus **busses)
{
  struct usb_bus *fbus = NULL;
  int controller;
  int fd;
  char buf[20];

  for (controller = 0; controller < MAX_CONTROLLERS; controller++) {
    struct usb_bus *bus;

    snprintf(buf, sizeof(buf) - 1, "/dev/usb%d", controller);
    fd = open(buf, O_RDWR);
    if (fd < 0) {
      if (usb_debug >= 2)
        if (errno != ENXIO && errno != ENOENT)
          fprintf(stderr, "usb_os_find_busses: can't open %s: %s\n",
                  buf, strerror(errno));
      continue;
    }
    close(fd);

    bus = malloc(sizeof(*bus));
    if (!bus)
      USB_ERROR(-ENOMEM);

    memset((void *)bus, 0, sizeof(*bus));

    strncpy(bus->dirname, buf, sizeof(bus->dirname) - 1);
    bus->dirname[sizeof(bus->dirname) - 1] = 0;

    LIST_ADD(fbus, bus);

    if (usb_debug >= 2)
      fprintf(stderr, "usb_os_find_busses: Found %s\n", bus->dirname);
  }

  *busses = fbus;

  return 0;
}

int usb_os_find_devices(struct usb_bus *bus, struct usb_device **devices)
{
  struct usb_device *fdev = NULL;
  int cfd, dfd;
  int device;

  cfd = open(bus->dirname, O_RDONLY);
  if (cfd < 0)
    USB_ERROR_STR(errno, "couldn't open(%s): %s", bus->dirname,
                  strerror(errno));

  for (device = 1; device < USB_MAX_DEVICES; device++) {
    struct usb_device_info di;
    struct usb_device *dev;
    struct usbi_device_priv *dpriv;
    char buf[20];

    di.udi_addr = device;
    if (ioctl(cfd, USB_DEVICEINFO, &di) < 0)
      continue;

    /* There's a device; is it one we should mess with? */

    if (strncmp(di.udi_devnames[0], "ugen", 4) != 0)
      /* best not to play with things we don't understand */
      continue;

#if __FreeBSD__
    snprintf(buf, sizeof(buf) - 1, "/dev/%s", di.udi_devnames[0]);
#else
    snprintf(buf, sizeof(buf) - 1, "/dev/%s.00", di.udi_devnames[0]);
#endif

    /* Open its control endpoint */
    dfd = open(buf, O_RDONLY);
    if (dfd < 0) {
      if (usb_debug >= 2)
        fprintf(stderr, "usb_os_find_devices: couldn't open device %s: %s\n",
                buf, strerror(errno));
      continue;
    }

    dev = malloc(sizeof(*dev));
    if (!dev)
      USB_ERROR(-ENOMEM);

    dpriv = malloc(sizeof(*dpriv));
    if (!dpriv) {
      free(dev);
      USB_ERROR(-ENOMEM);
    }

    memset((void *)dev, 0, sizeof(*dev));

    dev->priv = dpriv;
    dev->bus = bus;

    /* we need to report the device name as /dev/ugenx NOT /dev/ugenx.00
     * This seemed easier than having 2 variables...
     */
#if (__NetBSD__ || __OpenBSD__)
    snprintf(buf, sizeof(buf) - 1, "/dev/%s", di.udi_devnames[0]);
#endif

    /* FIXME: Find the device number and store it in the dev struct */
    strncpy(dpriv->filename, buf, sizeof(dpriv->filename) - 1);
    dpriv->filename[sizeof(dpriv->filename) - 1] = 0;

    if (ioctl(dfd, USB_GET_DEVICE_DESC, &dev->descriptor) < 0)
      USB_ERROR_STR(-errno, "couldn't get device descriptor for %s: %s",
                    buf, strerror(errno));

    close(dfd);

    usb_le16_to_cpu(&dev->descriptor.bcdUSB);
    usb_le16_to_cpu(&dev->descriptor.idVendor);
    usb_le16_to_cpu(&dev->descriptor.idProduct);
    usb_le16_to_cpu(&dev->descriptor.bcdDevice);

    LIST_ADD(fdev, dev);

    if (usb_debug >= 2)
      fprintf(stderr, "usb_os_find_devices: Found %s on %s\n",
              dpriv->filename, bus->dirname);
  }

  close(cfd);

  *devices = fdev;

  return 0;
}

void usb_os_init(void)
{
  /* nothing */
}

/* FIXME: Implement these for BSD */
int usb_clear_halt(usb_dev_handle *dev, unsigned char ep)
{
  /* Not yet done, because I haven't needed it. */

  USB_ERROR_STR(-ENOSYS, "usb_clear_halt called, unimplemented on BSD");
}

int usb_reset(usb_dev_handle *dev)
{
  /* Not yet done, because I haven't needed it. */

  USB_ERROR_STR(-ENOSYS, "usb_reset called, unimplemented on BSD");
}

