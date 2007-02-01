/*
 * Darwin/MacOS X Support
 *
 * (c) 2002 Nathan Hjelm <hjelmn@users.sourceforge.net>
 *
 * 0.1.6 (05/12/2002):
 *   - Fixed problem where libusb holds resources after program completion.
 *   - Mouse should no longer freeze up now.
 * 0.1.2 (02/13/2002):
 *   - Bulk functions should work properly now.
 * 0.1.1 (02/11/2002):
 *   - Fixed major bug (device and interface need to be released after use)
 * 0.1.0 (01/06/2002):
 *   - Tested driver with gphoto (works great as long as Image Capture isn't running)
 * 0.1d  (01/04/2002):
 *   - Implimented clear_halt and resetep
 *   - Uploaded to CVS.
 * 0.1b  (01/04/2002):
 *   - Added usb_debug line to bulk read and write function.
 * 0.1a  (01/03/2002):
 *   - Driver mostly completed using the macosx driver I wrote for my rioutil software.
 *
 * this driver is EXPERIMENTAL, use at your own risk and e-mail me bug reports.
 *
 * Derived from Linux version by Richard Tobin.
 * Also partly derived from BSD version.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "usbi.h"

#include <unistd.h>

/* standard includes for darwin/os10 (IOKit) */
#include <IOKit/IOCFBundle.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

/* some defines */
/* version 183 gives timeout bulk functions */
#define usb_interface_t IOUSBInterfaceInterface183
#define usb_device_t    IOUSBDeviceInterface182
#define io_return_t     IOReturn

/* Darwin/OS X impl does not use fd field, instead it uses this */
/* This will NOT work with 10.0 or darwin 1.4, upgrade! */
struct darwin_dev_handle {
  usb_device_t **device;
  usb_interface_t **interface;
};

static CFRunLoopSourceRef runLoopSource;
static masterPort = NULL;

int usb_os_open(usb_dev_handle *dev)
{
  struct darwin_dev_handle *device;

  io_return_t result;
  io_iterator_t deviceIterator;
  io_service_t usbDevice;

  usb_device_t **darwin_device;

  IOCFPlugInInterface **plugInInterface = NULL;
  IONotificationPortRef gNotifyPort;

  CFMutableDictionaryRef matchingDict;

  long score;
  u_int16_t address, vendorr;
  u_int32_t location;

  if (!dev)
    return -1;

  device = malloc(sizeof(struct darwin_dev_handle));
  if (!device)
    USB_ERROR(-ENOMEM);

  if (!masterPort)
    return NULL;

  /* set up the matching dictionary for class IOUSBDevice and it's subclasses */
  if ((matchingDict = IOServiceMatching(kIOUSBDeviceClassName)) == NULL) {
    mach_port_deallocate(mach_task_self(), masterPort);
    return NULL;
  }

  gNotifyPort = IONotificationPortCreate(masterPort);
  runLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);

  CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource,
                     kCFRunLoopDefaultMode);

  result = IOServiceAddMatchingNotification(gNotifyPort, kIOFirstMatchNotification,
                                            matchingDict, NULL, NULL,
                                            &deviceIterator);

  while (usbDevice = IOIteratorNext(deviceIterator)) {
    /* Create an intermediate plug-in */
    result = IOCreatePlugInInterfaceForService(usbDevice, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugInInterface,
                                               &score);

    result = IOObjectRelease(usbDevice);
    if (result || !plugInInterface)
      continue;

    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                       (LPVOID)&darwin_device);

    (*plugInInterface)->Release(plugInInterface);

    if (!darwin_device)
      continue;

    result = (*(darwin_device))->GetLocationID(darwin_device, (UInt32 *)&location);
    if (location == *((UInt32 *)dev->device->dev)) {
      device->device = darwin_device;
      break;
    } else {
      (*darwin_device)->Release(darwin_device);
      continue;
    }
  }

  IOObjectRelease(deviceIterator);
  deviceIterator = NULL;

  if (usb_debug > 3)
    fprintf(stderr, "usb_os_open: %04x:%04x\n",
	    dev->device->descriptor.idVendor,
	    dev->device->descriptor.idProduct);

  result = (*(device->device))->USBDeviceOpen(device->device);

  if (result)
    USB_ERROR_STR(result, "could not open device.");
  else
    dev->impl_info = device;

  return result;
}

int usb_os_close(usb_dev_handle *dev)
{
  struct darwin_dev_handle *device;
  io_return_t result;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  usb_release_interface(dev, dev->interface);

  if (usb_debug > 3)
    fprintf(stderr, "usb_os_close: %04x:%04x\n",
	    dev->device->descriptor.idVendor,
	    dev->device->descriptor.idProduct);

  result = (*(device->device))->USBDeviceClose(device->device);

  if (result)
    USB_ERROR_STR(result, "error closing device");

  /* device may not need to be released, but if it has too... */
  result = (*(device->device))->Release(device->device);

  if (result)
    fprintf(stderr, "error releasing device");

  return result;
}

int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
  struct darwin_dev_handle *device;
  io_return_t result;

  /* FIXME: Use standard return types, not hardcoded */
  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  result = (*(device->device))->SetConfiguration(device->device, configuration);

  if (result)
    USB_ERROR_STR(result, "could not set configuration");

  dev->config = configuration;

  return result;
}

int usb_claim_interface(usb_dev_handle *dev, int interface)
{
  struct darwin_dev_handle *device = dev->impl_info;

  io_iterator_t interface_iterator;
  io_service_t  usbInterface;

  io_return_t result;
  IOUSBFindInterfaceRequest request;
  IOCFPlugInInterface **plugInInterface = NULL;

  long score;
  int current_interface = 0;

  request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
  request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
  request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
  request.bAlternateSetting = kIOUSBFindInterfaceDontCare;

  if (!device)
    return -1;

  if (!(device->device))
    return -2;

  (*(device->device))->CreateInterfaceIterator(device->device, &request, &interface_iterator);

  while (current_interface++ <= interface)
    usbInterface = IOIteratorNext(interface_iterator);

  /* the interface iterator is no longer needed, release it */
  IOObjectRelease(interface_iterator);
  interface_iterator = NULL; /* not really needed */

  if (!usbInterface)
    return -1;

  result = IOCreatePlugInInterfaceForService(usbInterface,
						   kIOUSBInterfaceUserClientTypeID,
						   kIOCFPlugInInterfaceID,
						   &plugInInterface, &score);

  //No longer need the usbInterface object after getting the plug-in
  result = IOObjectRelease(usbInterface);
  if (result || !plugInInterface)
    return -1;

  //Now create the device interface for the interface
  result = (*plugInInterface)->QueryInterface(plugInInterface,
                                              CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
                                              (LPVOID) &device->interface);
  //No longer need the intermediate plug-in
  (*plugInInterface)->Release(plugInInterface);

  if (result || !device->interface)
    return -1;

  /* claim the interface */
  result = (*(device->interface))->USBInterfaceOpen(device->interface);
  if (result)
    return -1;

  result = (*(device->interface))->CreateInterfaceAsyncEventSource(device->interface, &runLoopSource);
  if (result)
    return -1;
  CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource, kCFRunLoopDefaultMode);

  dev->interface = interface;

  /* interface is claimed and async IO is set up return 0 */
  return result;
}

int usb_release_interface(usb_dev_handle *dev, int interface)
{
  struct darwin_dev_handle *device;
  io_return_t result;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  /* interface is not open */
  if (!device->interface)
    return 0;

  result = (*(device->interface))->USBInterfaceClose(device->interface);

  if (result)
    USB_ERROR_STR(result, "error closing interface");

  result = (*(device->interface))->Release(device->interface);

  if (result)
    USB_ERROR_STR(result, "error releasing interface");

  device->interface = NULL;

  return result;
}

int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
  struct darwin_dev_handle *device;
  io_return_t result;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  /* interface is not open */
  if (!device->interface) {
    USB_ERROR_STR(0, "interface used without being claimed");
    return -1;
  }

  result = (*(device->interface))->SetAlternateInterface(device->interface, alternate);

  if (result)
    USB_ERROR_STR(result, "sould not set alternate interface");

  dev->altsetting = alternate;

  return result;
}

/* simple function that figures out what pipeRef is associated with an endpoint */
static int ep_to_pipeRef (struct darwin_dev_handle *device, int ep)
{
  u_int8_t numep, direction, number;
  u_int8_t dont_care1, dont_care3;
  u_int16_t dont_care2;
  int i;

  if (usb_debug)
    fprintf(stderr, "Converting ep address to pipeRef.\n");

  /* retrieve the total number of endpoints on this interface */
  (*(device->interface))->GetNumEndpoints(device->interface, &numep);

  /* iterate through the pipeRefs until we find the correct one */
  for (i = 1 ; i <= numep ; i++) {
    (*(device->interface))->GetPipeProperties(device->interface, i, &direction, &number,
					      &dont_care1, &dont_care2, &dont_care3);

    /* calculate the endpoint of the pipe and check it versus the requested endpoint */
    if ( (direction << 7 | number) == ep ) {
      if (usb_debug)
	fprintf(stderr, "pipeRef for ep 0x%02x found: 0x%02x\n", ep, i);

      return i;
    }
  }

  if (usb_debug)
    fprintf(stderr, "No pipeRef found with endpoint address 0x%02x.\n", ep);

  /* none of the found pipes match the requested endpoint */
  return -1;
}

static void write_completed(void *dummy, io_return_t result, void *arg0)
{
  if (usb_debug > 2)
    fprintf(stderr, "write completed\n");

  CFRunLoopStop(CFRunLoopGetCurrent());
}

/* these will require lots of work */
int usb_bulk_write(usb_dev_handle *dev, unsigned char ep, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct darwin_dev_handle *device;
  CFRunLoopSourceRef cfSource;
  io_return_t result = -1;
  int pipeRef;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  /* interface is not open */
  if (!device->interface) {
    USB_ERROR_STR(0, "device used without claiming an interface");
    return -1;
  }

  if ((pipeRef = ep_to_pipeRef(device, ep)) == -1)
    return -1;

  if (usb_debug > 3)
    fprintf(stderr, "usb_bulk_write: endpoint=0x%02x size=%i TO=%i\n", ep, size, timeout);

  (*(device->interface))->CreateInterfaceAsyncEventSource(device->interface, &cfSource);
  CFRunLoopAddSource(CFRunLoopGetCurrent(), cfSource, kCFRunLoopDefaultMode);

  /* there seems to be no way to determine how many bytes are actually written */
  result = (*(device->interface))->WritePipeAsyncTO(device->interface, pipeRef,
					    bytes, numbytes, 0, timeout,
					    write_completed, NULL);

  /* wait for write to complete */
  CFRunLoopRun();

  if (usb_debug)
    fprintf(stderr, "CFLoopRun returned\n");

  if (result)
    USB_ERROR_STR(result, "error writing to device.");

  return result;
}

int usb_bulk_read(usb_dev_handle *dev, unsigned char ep, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct darwin_dev_handle *device;
  int pipeRef;
  u_int32_t ret_size = size;
  u_int32_t retrieved = 0;
  io_return_t result = -1;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  /* interface is not open */
  if (!device->interface) {
    USB_ERROR_STR(0, "interface used without being claimed");
    return -1;
  }

  if ((pipeRef = ep_to_pipeRef(device, ep)) == -1)
    return -1;

  if (usb_debug > 3)
    fprintf(stderr, "usb_bulk_read: endpoint=0x%02x size=%i TO=%i\n", ep, size, timeout);

  /* now in a loop, this should fix the problems reading large amounts of data from
     the bulk endpoint */
  do {
    result = (*(device->interface))->ReadPipeTO(device->interface, pipeRef,
					     bytes + retrieved, (UInt32 *)&ret_size, 0, timeout);
    if (result)
      USB_ERROR_STR(result, "error reading from bulk endpoint %02x", ep);

    retrieved += ret_size;
    ret_size = numbytes - retrieved;
  } while (!result && retrieved < numbytes);

  if (result || (ret_size < 0))
    USB_ERROR_STR(0, "error reading from endpoint %02x", ep);

  retrieved += ret_size;

  return retrieved;
}

int usb_control_msg(usb_dev_handle *dev, uint8_t bRequestType, uint8_t bRequest,
	uint16_t wValue, uint16_t wIndex, unsigned char *bytes,
	unsigned int numbytes, unsigned int timeout)
{
  struct darwin_dev_handle *device = dev->impl_info;

  IOUSBDevRequestTO urequest;
  io_return_t result;

  if (usb_debug >= 3)
    fprintf(stderr, "usb_control_msg: %d %d %d %d %p %d %d\n",
            requesttype, request, value, index, bytes, numbytes, timeout);

  bzero(&urequest, sizeof(IOUSBDevRequestTO));

  urequest.bmRequestType = bRequestType;
  urequest.bRequest = bRequest;
  urequest.wValue = wValue;
  urequest.wIndex = wIndex;
  urequest.wLength = numbytes;
  urequest.pData = bytes;
  urequest.completionTimeout = timeout;

  result = (*(device->device))->DeviceRequestTO(device->device, &urequest);

  if (result) {
    USB_ERROR_STR(result, "error sending control message.");
    return -1;
  }

  /* i am pretty sure DeviceRequest modifies this */
  return urequest.wLength;
}

int usb_os_find_busses(struct usb_bus **busses)
{
  struct usb_bus *fbus = NULL;

  io_iterator_t deviceIterator;
  io_service_t usbDevice;

  usb_device_t **device;

  IOCFPlugInInterface **plugInInterface = NULL;
  IONotificationPortRef gNotifyPort;

  CFMutableDictionaryRef matchingDict;

  io_return_t result;
  SInt32 score;
  UInt16 address;
  UInt32 location;

  char buf[20];
  int busnum = 0;

  /* Create a master port for communication with IOKit (this should
   have been created if the user called usb_init() )*/
  if (!masterPort) {
    result = IOMasterPort(MACH_PORT_NULL, &masterPort);

    if (result || !masterPort)
      return NULL;
  }

  /* set up the matching dictionary for class IOUSBDevice and it's subclasses */
  if ((matchingDict = IOServiceMatching(kIOUSBDeviceClassName)) == NULL) {
    mach_port_deallocate(mach_task_self(), masterPort);
    return NULL;
  }

  gNotifyPort = IONotificationPortCreate(masterPort);
  runLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);

  CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource,
                     kCFRunLoopDefaultMode);

  result = IOServiceAddMatchingNotification(gNotifyPort, kIOFirstMatchNotification,
                                            matchingDict, NULL, NULL,
                                            &deviceIterator);

  while (usbDevice = IOIteratorNext(deviceIterator)) {
    struct usb_bus *bus;

    /* Create an intermediate plug-in */
    result = IOCreatePlugInInterfaceForService(usbDevice, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugInInterface,
                                               &score);

    result = IOObjectRelease(usbDevice);
    if (result || !plugInInterface)
      continue;

    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                       (LPVOID)&device);

    /* done with this */
    (*plugInInterface)->Release(plugInInterface);

    if (!device)
      continue;

    result = (*(device))->GetDeviceAddress(device, (USBDeviceAddress *)&address);
    result = (*(device))->GetLocationID(device, &location);

    if (address == 0x1) {
      bus = malloc(sizeof(*bus));
      if (!bus)
	USB_ERROR(-ENOMEM);

      memset((void *)bus, 0, sizeof(*bus));

      sprintf(buf, "%03i", ( (location & 0x0f000000) >> 24) - 0x8);
      strncpy(bus->dirname, buf, sizeof(bus->dirname) - 1);
      bus->dirname[sizeof(bus->dirname) - 1] = 0;

      LIST_ADD(fbus, bus);

      if (usb_debug >= 2)
	fprintf(stderr, "usb_os_find_busses: Found %s\n", bus->dirname);

      (*(device))->Release(device);
    }
  }

  IOObjectRelease(deviceIterator);

  *busses = fbus;

  return 0;
}

int usb_os_find_devices(struct usb_bus *bus, struct usb_device **devices)
{
  struct usb_device *fdev = NULL;

  io_iterator_t deviceIterator;
  io_service_t usbDevice;

  usb_device_t **device;

  IOCFPlugInInterface **plugInInterface = NULL;
  IONotificationPortRef gNotifyPort;

  CFMutableDictionaryRef matchingDict;

  io_return_t result;
  SInt32 score;
  UInt16 address;
  UInt32 location;
  UInt32 bus_loc = atoi(bus->dirname);

  /* for use in retrieving device description */
  IOUSBDevRequest req;

  char buf[20];
  int busnum = 0;

  /* FIXME: Don't return NULL into an int return type */

  /* a master port should have been created by usb_os_init */
  if (!masterPort)
    return NULL;

  /* set up the matching dictionary for class IOUSBDevice and it's subclasses */
  if ((matchingDict = IOServiceMatching(kIOUSBDeviceClassName)) == NULL) {
    mach_port_deallocate(mach_task_self(), masterPort);
    return NULL;
  }

  gNotifyPort = IONotificationPortCreate(masterPort);
  runLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);

  CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource,
                     kCFRunLoopDefaultMode);

  result = IOServiceAddMatchingNotification(gNotifyPort, kIOFirstMatchNotification,
                                            matchingDict, NULL, NULL,
                                            &deviceIterator);

  req.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
  req.bRequest = kUSBRqGetDescriptor;
  req.wValue = kUSBDeviceDesc << 8;
  req.wIndex = 0;
  req.wLength = sizeof(IOUSBDeviceDescriptor);

  while (usbDevice = IOIteratorNext(deviceIterator)) {
    /* Create an intermediate plug-in */
    result = IOCreatePlugInInterfaceForService(usbDevice, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugInInterface,
                                               &score);

    result = IOObjectRelease(usbDevice);
    if (result || !plugInInterface)
      continue;

    (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                       (LPVOID)&device);

    /* done with this */
    (*plugInInterface)->Release(plugInInterface);

    if (!device)
      continue;

    result = (*(device))->GetDeviceAddress(device, (USBDeviceAddress *)&address);
    result = (*(device))->GetLocationID(device, &location);

    if ((location & 0x0f000000) == ((bus_loc + 0x8) << 24)) {
      struct usb_device *dev;

      dev = malloc(sizeof(*dev));
      if (!dev)
	USB_ERROR(-ENOMEM);

      memset((void *)dev, 0, sizeof(*dev));

      dev->bus = bus;

      dev->devnum = address;
      req.pData = &(dev->descriptor);
      result = (*(device))->DeviceRequest(device, &req);

      usb_le16_to_cpu(&dev->descriptor.bcdUSB);
      usb_le16_to_cpu(&dev->descriptor.idVendor);
      usb_le16_to_cpu(&dev->descriptor.idProduct);
      usb_le16_to_cpu(&dev->descriptor.bcdDevice);

      dev->dev = (USBDeviceAddress *)malloc(4);
      memcpy(dev->dev, &location, 4);

      LIST_ADD(fdev, dev);

      if (usb_debug >= 2)
	fprintf(stderr, "usb_os_find_devices: Found %d on %s\n",
		dev->devnum, bus->dirname);

      /* release the device now */
      (*(device))->Release(device);
    }
  }

  IOObjectRelease(deviceIterator);

  *devices = fdev;

  return 0;
}

void usb_os_init(void)
{
  IOMasterPort(MACH_PORT_NULL, &masterPort);
}

int usb_clear_halt(usb_dev_handle *dev, unsigned char ep)
{
  struct darwin_dev_handle *device;
  io_return_t result = -1;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -2;

  /* interface is not open */
  if (!device->interface) {
    USB_ERROR_STR(0, "interface used without being claimed");
    return -1;
  }

  result = (*(device->interface))->ClearPipeStall(device->interface, ep);

  if (result)
    USB_ERROR_STR(result, "error clearing pipe stall");

  return result;
}

int usb_reset(usb_dev_handle *dev)
{
  struct darwin_dev_handle *device;

  if (!dev)
    return -1;

  if ((device = dev->impl_info) == NULL)
    return -1;

  if (device->device)
    (*(device->device))->ResetDevice(device->device);
  else
    USB_ERROR_STR(-1, "no such device");

  return 0;
}

