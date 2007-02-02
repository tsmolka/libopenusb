/*
 * Handling of asynchronous IO with devices
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include "usbi.h"

int usbi_sync_ctrl_submit(struct libusb_ctrl_request *ctrl,
	size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  unsigned char *setup;
  int ret;

  dev = usbi_find_dev_handle(ctrl->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  setup = malloc(USBI_CONTROL_SETUP_LEN);
  if (!setup)
    return LIBUSB_NO_RESOURCES;

  /* Fill in the SETUP packet */
  setup[0] = ctrl->setup.bRequestType;
  setup[1] = ctrl->setup.bRequest;
  *(uint16_t *)(setup + 2) = libusb_cpu_to_le16(ctrl->setup.wValue);
  *(uint16_t *)(setup + 4) = libusb_cpu_to_le16(ctrl->setup.wIndex);
  *(uint16_t *)(setup + 6) = libusb_cpu_to_le16(ctrl->buflen);

  io = usbi_alloc_io(ctrl->dev, USBI_IO_CONTROL, ctrl->endpoint, ctrl->timeout);
  if (!io) {
    free(setup);
    return LIBUSB_NO_RESOURCES;
  }

  io->ctrl.request = ctrl;
  io->ctrl.callback = NULL;
  io->ctrl.arg = NULL;

  io->ctrl.setup = setup;
  io->ctrl.buf = ctrl->buf;
  io->ctrl.buflen = ctrl->buflen;

  ret = dev->idev->ops->submit_ctrl(dev, io);
  usbi_free_io(io);
  if (ret < 0) {
    return ret;
  }

  *transferred_bytes = ret;

  return LIBUSB_SUCCESS;
}

int usbi_sync_intr_submit(struct libusb_intr_request *intr,
	size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  int ret;

  dev = usbi_find_dev_handle(intr->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io = usbi_alloc_io(intr->dev, USBI_IO_INTERRUPT, intr->endpoint, intr->timeout);
  if (!io)
    return LIBUSB_NO_RESOURCES;

  io->intr.request = intr;
  io->intr.callback = NULL;
  io->intr.arg = NULL;

  io->intr.buf = intr->buf;
  io->intr.buflen = intr->buflen;

  ret = dev->idev->ops->submit_intr(dev, io);
  usbi_free_io(io);
  if (ret < 0) {
    return ret;
  }

  *transferred_bytes = ret;

  return LIBUSB_SUCCESS;
}

int usbi_sync_bulk_submit(struct libusb_bulk_request *bulk,
	size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  int ret;

  dev = usbi_find_dev_handle(bulk->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io = usbi_alloc_io(bulk->dev, USBI_IO_BULK, bulk->endpoint, bulk->timeout);
  if (!io)
    return LIBUSB_NO_RESOURCES;

  io->bulk.request = bulk;
  io->bulk.callback = NULL;
  io->bulk.arg = NULL;

  io->bulk.buf = bulk->buf;
  io->bulk.buflen = bulk->buflen;

  ret = dev->idev->ops->submit_bulk(dev, io);
  usbi_free_io(io);
  if (ret < 0) {
    return ret;
  }

  *transferred_bytes = ret;

  return LIBUSB_SUCCESS;
}

int usbi_sync_isoc_submit(struct libusb_isoc_request *iso, void *arg)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  int ret;

  dev = usbi_find_dev_handle(iso->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io = usbi_alloc_io(iso->dev, USBI_IO_ISOCHRONOUS, iso->endpoint, 0);
  if (!io)
    return LIBUSB_NO_RESOURCES;

  io->isoc.request = iso;
  io->isoc.callback = NULL;
  io->isoc.arg = arg;

  ret = dev->idev->ops->submit_isoc(dev, io);
  usbi_free_io(io);
  if (ret < 0) {
    return ret;
  }

  return LIBUSB_SUCCESS;
}
