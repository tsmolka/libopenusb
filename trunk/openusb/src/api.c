/*
 * API implementation
 *
 * Copyright 2006 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include "usbi.h"

int libusb_set_configuration(libusb_device_id_t devid, int cfg)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  return idev->ops->set_configuration(idev, cfg);
}

int libusb_get_configuration(libusb_device_id_t devid, int *cfg)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  return idev->ops->get_configuration(idev, cfg);
}

int libusb_claim_interface(libusb_dev_handle_t dev, int interface)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->claim_interface(hdev, interface);
}

int libusb_release_interface(libusb_dev_handle_t dev, int interface)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->release_interface(hdev, interface);
}

int libusb_set_altinterface(libusb_dev_handle_t dev, int alt)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->set_altinterface(hdev, alt);
}

int libusb_get_altinterface(libusb_device_id_t devid, int *alt)
{
  struct usbi_device *idev;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  return idev->ops->get_altinterface(idev, alt);
}

int libusb_reset(libusb_dev_handle_t dev)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->reset(hdev);
}

int libusb_attach_kernel_driver_np(libusb_dev_handle_t dev, int interface)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->attach_kernel_driver_np(hdev, interface);
}

int libusb_detach_kernel_driver_np(libusb_dev_handle_t dev, int interface)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  return hdev->idev->ops->detach_kernel_driver_np(hdev, interface);
}

