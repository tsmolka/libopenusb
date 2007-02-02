/*
 * print_tree.c
 *
 *  Example to enumerate all devices and display a tree
 */

#include <stdio.h>
#include <libusb.h>
#include <errno.h>
#include <string.h>

void print_endpoint(struct usb_endpoint_desc *ep)
{ 
  printf("      bEndpointAddress: %02xh\n", ep->bEndpointAddress);
  printf("      bmAttributes:     %02xh\n", ep->bmAttributes);
  printf("      wMaxPacketSize:   %d\n", ep->wMaxPacketSize);
  printf("      bInterval:        %d\n", ep->bInterval);
  printf("      bRefresh:         %d\n", ep->bRefresh);
  printf("      bSynchAddress:    %d\n", ep->bSynchAddress);
}
  
void print_interface(libusb_device_id_t devid, int cfgidx, int ifcidx,
	struct usb_interface_desc *intf)
{ 
  int i;
  
  printf("    bInterfaceNumber:   %d\n", intf->bInterfaceNumber);
  printf("    bAlternateSetting:  %d\n", intf->bAlternateSetting);
  printf("    bNumEndpoints:      %d\n", intf->bNumEndpoints);
  printf("    bInterfaceClass:    %d\n", intf->bInterfaceClass);
  printf("    bInterfaceSubClass: %d\n", intf->bInterfaceSubClass);
  printf("    bInterfaceProtocol: %d\n", intf->bInterfaceProtocol);
  printf("    iInterface:         %d\n", intf->iInterface);

  for (i = 0; i < intf->bNumEndpoints; i++) {
    struct usb_endpoint_desc ep;

    libusb_get_endpoint_desc(devid, cfgidx, ifcidx, i, &ep);
    print_endpoint(&ep);
  }
}

void print_configuration(libusb_device_id_t devid, int cfgidx,
	struct usb_config_desc *cfg)
{
  int i;

  printf("  wTotalLength:         %d\n", cfg->wTotalLength);
  printf("  bNumInterfaces:       %d\n", cfg->bNumInterfaces);
  printf("  bConfigurationValue:  %d\n", cfg->bConfigurationValue);
  printf("  iConfiguration:       %d\n", cfg->iConfiguration);
  printf("  bmAttributes:         %02xh\n", cfg->bmAttributes);
  printf("  MaxPower:             %d\n", cfg->MaxPower);

  for (i = 0; i < cfg->bNumInterfaces; i++) {
    struct usb_interface_desc intf;

    libusb_get_interface_desc(devid, cfgidx, i, &intf);
    print_interface(devid, cfgidx, i, &intf);
  }
}

void print_device(libusb_device_id_t devid, int indent)
{
  struct usb_device_desc dev;
  unsigned char devnum;
  int i, ret;

  libusb_get_devnum(devid, &devnum);
  printf("%.*s+ device #%d\n", indent * 2, "                ", devnum);

  if ((ret = libusb_get_device_desc(devid, &dev)) != 0) {
    printf("get device desc fail, ret = %d\n", ret);
    return;
  }

  printf("bcdUSB:                 %04xh\n", dev.bcdUSB);
  printf("bDeviceClass:           %d\n", dev.bDeviceClass);
  printf("bDeviceSubClass:        %d\n", dev.bDeviceSubClass);
  printf("bDeviceProtocol:        %d\n", dev.bDeviceProtocol);
  printf("bMaxPacketSize0:        %d\n", dev.bMaxPacketSize0);
  printf("idVendor:               %04xh\n", dev.idVendor);
  printf("idProduct:              %04xh\n", dev.idProduct);
  printf("bcdDevice:              %04xh\n", dev.bcdDevice);
  printf("iManufacturer:          %d\n", dev.iManufacturer);
  printf("iProduct:               %d\n", dev.iProduct);
  printf("iSerialNumber:          %d\n", dev.iSerialNumber);
  printf("bNumConfigurations:     %d\n", dev.bNumConfigurations);

  for (i = 0; i < dev.bNumConfigurations; i++) {
    struct usb_config_desc cfg;

    if ((ret = libusb_get_config_desc(devid, i, &cfg)) != 0) {
      printf("get config desc fail, ret = %d\n", ret);
      return;
    }
    print_configuration(devid, i, &cfg);
  }

  printf("\n");
}

void print_device_and_walk(libusb_device_id_t devid, int indent)
{
  unsigned char count;
  int i;

  print_device(devid, indent);

  libusb_get_child_count(devid, &count);
  for (i = 0; i < count; i++) {
    libusb_device_id_t cdevid;

    if (!libusb_get_child_device_id(devid, i + 1, &cdevid))
      print_device_and_walk(cdevid, indent + 1);
  }
}

void print_bus_and_walk(libusb_bus_id_t busid)
{
  libusb_device_id_t devid;

  printf("bus #%d\n", busid);

  libusb_get_first_device_id(busid, &devid);
  print_device_and_walk(devid, 0);
}

int main(void)
{
  libusb_bus_id_t busid;

  if (libusb_init() != LIBUSB_SUCCESS) {
	printf("libusb init failed\n");
	return -1;
  }

  if (libusb_get_first_bus_id(&busid) == LIBUSB_FAILURE) {
	printf("no USB bus found\n");
	return -1;
  }

  do {
    print_bus_and_walk(busid);
  } while (libusb_get_next_bus_id(&busid) == LIBUSB_SUCCESS);

  return 0;
}

