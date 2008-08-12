/*
 * Darwin IOKit (Mac OS X) USB Support
 *
 *  Copyright 2002-2008 Nathan Hjelm <hjelmn@users.sourceforge.net>
 *  Copyright 2008 Michael Lewis <milewis1@gmail.com>
 *
 *	This library is covered by the LGPL, read LICENSE for details.
 */

#include <mach/mach_port.h>
#include <IOKit/IOCFBundle.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

#include <errno.h>

#include "usbi.h"
#include "darwin.h"

static CFRunLoopRef openusb_darwin_acfl = NULL; /* async cf loop */
static mach_port_t  openusb_darwin_mp = 0; /* master port */
static pthread_t    openusb_darwin_at; /* async thread */

static int32_t process_new_device (struct usbi_bus *ibus, usb_device_t **device, UInt32 locationID);

static char *darwin_error_str (int result) {
  switch (result) {
  case kIOReturnSuccess:
    return "no error";
  case kIOReturnNotOpen:
    return "device not opened for exclusive access";
  case kIOReturnNoDevice:
    return "no connection to an IOService";
  case kIOUSBNoAsyncPortErr:
    return "no async port has been opened for interface";
  case kIOReturnExclusiveAccess:
    return "another process has device opened for exclusive access";
  case kIOUSBPipeStalled:
    return "pipe is stalled";
  case kIOReturnError:
    return "could not establish a connection to the Darwin kernel";
  case kIOUSBTransactionTimeout:
    return "transaction timed out";
  case kIOReturnBadArgument:
    return "invalid argument";
  case kIOReturnAborted:
    return "transaction aborted";
  case kIOReturnNotResponding:
    return "device not responding";
  default:
    return "unknown error";
  }
}

static int darwin_to_openusb (int result) {
  switch (result) {
  case kIOReturnSuccess:
    return OPENUSB_SUCCESS;
  case kIOReturnNotOpen:
  case kIOReturnNoDevice:
    return OPENUSB_UNKNOWN_DEVICE;
  case kIOReturnExclusiveAccess:
    return OPENUSB_NOACCESS;
  case kIOUSBPipeStalled:
    return OPENUSB_IO_STALL;
  case kIOReturnBadArgument:
    return OPENUSB_BADARG;
  case kIOUSBTransactionTimeout:
    return OPENUSB_IO_TIMEOUT;
  case kIOReturnNotResponding:
    return OPENUSB_IO_DEVICE_HUNG;
  case kIOReturnAborted:
    return OPENUSB_IO_CANCELED;
  case kIOReturnError:
  case kIOUSBNoAsyncPortErr:
  default:
    return OPENUSB_PLATFORM_FAILURE;
  }
}

static int usb_setup_device_iterator (io_iterator_t *deviceIterator) {
  kern_return_t kresult;

  kresult = IOServiceGetMatchingServices(openusb_darwin_mp, IOServiceMatching(kIOUSBDeviceClassName), deviceIterator);

  if (kresult != kIOReturnSuccess) {
    usbi_debug (NULL, 1, "libopenusb/darwin.c usb_setup_device_iterator: IOServiceGetMatchingServices: %s",
		darwin_error_str(kresult));

    return kresult;
  }

  return 0;
}

static usb_device_t **usb_get_next_device (io_iterator_t deviceIterator, UInt32 *locationp) {
  io_cf_plugin_ref_t *plugInInterface = NULL;
  usb_device_t **device;
  io_service_t usbDevice;
  long result, score;

  if (!IOIteratorIsValid (deviceIterator) || !(usbDevice = IOIteratorNext(deviceIterator)))
    return NULL;
  
  result = IOCreatePlugInInterfaceForService(usbDevice, kIOUSBDeviceUserClientTypeID,
					     kIOCFPlugInInterfaceID, &plugInInterface,
					     &score);
  
  if (result || !plugInInterface) {
    usbi_debug (NULL, 1,
		"libopenusb/darwin.c usb_get_next_device: could not set up plugin for service: %s\n",
		darwin_error_str (result));
    return NULL;
  }

  (void)IOObjectRelease(usbDevice);
  (void)(*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(DeviceInterfaceID),
					   (LPVOID)&device);

  (*plugInInterface)->Stop(plugInInterface);
  IODestroyPlugInInterface (plugInInterface);
  
  (*(device))->GetLocationID(device, locationp);

  return device;
}


static struct usbi_device *darwin_find_device_by_location (UInt32 location) {
  struct usbi_list    *pusbi_devices = usbi_get_devices_list();
  struct usbi_device  *idev = NULL;

  list_for_each_entry(idev, &((*pusbi_devices).head), dev_list) {
    if (idev->priv->location == location)
      return idev;
  }

  return NULL;
}

static void darwin_devices_attached (void *ptr, io_iterator_t new_devices) {
  usb_device_t **device;
  UInt32 location;
  struct usbi_bus *ibus = NULL;
  struct usbi_handle *handle, *thdl;
  struct usbi_device  *idev = NULL;

  while ((device = usb_get_next_device (new_devices, &location)) != NULL) {
    idev = darwin_find_device_by_location (location);

    if (idev) {
      /* old device re-inserted -- this likely can not happen */
      fprintf (stderr, "Old device reinserted\n");
      usbi_debug(NULL, 4, "old device: %d", (int)idev->devid);
      pthread_mutex_lock(&usbi_handles.lock);

      list_for_each_entry_safe(handle, thdl, &usbi_handles.head, list) {
	/* every openusb instance should get notification
	 * of this event */
	usbi_add_event_callback(handle, idev->devid, USB_ATTACH);
      }

      pthread_mutex_unlock(&usbi_handles.lock);
    } else {
      ibus = usbi_find_bus_by_num(location >> 24);

      if (!ibus) {
	usbi_debug(NULL, 4, "Unable to find bus by number: %d", location >> 24);
	return;
      }

      process_new_device (ibus, device, location);
    }

    (*device)->Release (device);
  }
}

static void darwin_devices_detached (void *ptr, io_iterator_t rem_devices) {  
  io_service_t device;
  struct usbi_device  *idev = NULL;
  long location;
  CFTypeRef locationCF;

  while ((device = IOIteratorNext (rem_devices)) != 0) {
    /* get the location from the registry */
    locationCF = IORegistryEntryCreateCFProperty (device, CFSTR(kUSBDevicePropertyLocationID), kCFAllocatorDefault, 0);

    CFNumberGetValue(locationCF, kCFNumberLongType, &location);
    CFRelease (locationCF);

    IOObjectRelease (device);

    idev = darwin_find_device_by_location (location);
    if (idev)
      usbi_remove_device (idev);

    pthread_mutex_unlock(&usbi_devices.lock);

  }
}

static void darwin_clear_iterator (io_iterator_t iter) {
  io_service_t device;

  while ((device = IOIteratorNext (iter)) != 0)
    IOObjectRelease (device);
}

static void *event_thread_main (void *ptr) {	
  io_notification_port_t notify_port;
  kern_return_t kresult;
  io_iterator_t new_device, rem_device;
  CFRunLoopSourceRef notification_cfsource;
  mach_port_t thread_port;

  IOMasterPort (MACH_PORT_NULL, &thread_port);
  
  notify_port = IONotificationPortCreate (thread_port);

  /* add the notification port to the current run loop */
  notification_cfsource = IONotificationPortGetRunLoopSource (notify_port);
  CFRunLoopAddSource(CFRunLoopGetCurrent(), notification_cfsource, kCFRunLoopDefaultMode);

  /* create notifications for new devices */
  kresult = IOServiceAddMatchingNotification (notify_port, kIOFirstMatchNotification,
					      IOServiceMatching(kIOUSBDeviceClassName),
					      (IOServiceMatchingCallback)darwin_devices_attached,
					      ptr, &new_device);
  if (kresult) {
    usbi_debug (NULL, 1,
		"libopenusb/darwin.c event_thread_main: could not set up device attach notifications: %s",
		darwin_error_str (kresult));
  }

  /* create notifications for removed devices */
  kresult = IOServiceAddMatchingNotification (notify_port, kIOTerminatedNotification,
					      IOServiceMatching(kIOUSBDeviceClassName),
					      (IOServiceMatchingCallback)darwin_devices_detached,
					      ptr, &rem_device);
 
  if (kresult) {
    usbi_debug (NULL, 1,
		"libopenusb/darwin.c event_thread_main: could not set up device dettach notifications: %s",
		darwin_error_str (kresult));
  }

  /* arm notifiers */
  darwin_clear_iterator (new_device);
  darwin_clear_iterator (rem_device);

  /* let the main thread know about the async runloop */
  openusb_darwin_acfl = CFRunLoopGetCurrent ();

  usbi_debug (NULL, 4, "libopenusb/darwin.c event_thread_main: thread ready to receive events");

  /* run the runloop */
  CFRunLoopRun();

  usbi_debug (NULL, 4, "libopenusb/darwin.c event_thread_main: thread exiting");

  /* delete notification port */
  CFRunLoopSourceInvalidate (notification_cfsource);
  CFRelease (notification_cfsource);
  
  openusb_darwin_acfl = NULL;

  pthread_exit (0);
}

/* darwin_init
 *
 *  Backend initialization, called in openusb_init()
 *    flags - inherited from openusb_init(), TBD
 */
int32_t darwin_init (struct usbi_handle *hdl, uint32_t flags) {
  kern_return_t	result;

  /* Validate... */
  if (!hdl)
    return OPENUSB_BADARG;

  /* Create the master port for talking to IOKit */
  if (!openusb_darwin_mp) {
    result = IOMasterPort(MACH_PORT_NULL, &openusb_darwin_mp);

    if (result || !openusb_darwin_mp)
      return OPENUSB_PLATFORM_FAILURE;
  }

  pthread_create (&openusb_darwin_at, NULL, event_thread_main, (void *)hdl);

  /* wait for event thread to initialize */
  while (1) {
    if (openusb_darwin_acfl != NULL)
      break;

    /* sleep for a bit */
    usleep (10);
  }

  return OPENUSB_SUCCESS;
}



/*
 * darwin_fini
 *
 *  Backend specific data cleanup, called in openusb_fini()
 */
void darwin_fini(struct usbi_handle *hdl) {
  void *ret;
	
  /* Make the master port NULL so we know we're closed */
  if (openusb_darwin_mp) {
    mach_port_deallocate(mach_task_self(), openusb_darwin_mp);
  }

  openusb_darwin_mp = 0;
	
  /* stop the async runloop */
  CFRunLoopStop (openusb_darwin_acfl);
  pthread_join (openusb_darwin_at, &ret);
	
  return;
}



/* darwin_find_busses
 *
 *  Seearch USB buses under the control of the backend and return the bus list
 */
int32_t darwin_find_busses(struct list_head *buses) {
  struct usbi_bus *ibus = NULL;
  io_iterator_t   deviceIterator;
  usb_device_t    **device;
  kern_return_t   kresult;
  char            buf[20];
  int             busnum = 1;
  UInt32          location;

  /* Validate... */
  if (!buses) 
    return OPENUSB_BADARG;

  kresult = usb_setup_device_iterator (&deviceIterator);
  if (kresult)
    return kresult;

  while ((device = usb_get_next_device (deviceIterator, &location)) != NULL) {
    if (location & 0x00ffffff)
      continue;

    ibus = calloc (1, sizeof(struct usbi_bus));

    if (!ibus) {
      usbi_debug(NULL, 1,
		 "libopenusb/darwin.c darwin_find_busses: could not allocation memory for usbi_bus");
      (*(device))->Release(device);
      return (OPENUSB_NO_RESOURCES);
    }
		
    ibus->priv = calloc (1, sizeof (struct usbi_bus_private));
		
    if (!ibus->priv) {
      usbi_debug(NULL, 1,
		 "libopenusb/darwin.c darwin_find_busses: could not allocation memory for usbi_bus_private");
      free (ibus);
      (*(device))->Release(device);
      return (OPENUSB_NO_RESOURCES);
    }

    ibus->busnum = location >> 24;
    ibus->priv->location = location;
		
    /* Set the system path of this bus */
    sprintf(buf, "%03i", busnum++);
    strncpy(ibus->sys_path, buf, sizeof(ibus->sys_path) - 1);

    /* Initialize our mutexes */
    pthread_mutex_init(&ibus->lock, NULL);
    pthread_mutex_init(&ibus->devices.lock, NULL);

    /* add this bus to our list */
    list_add(&ibus->list, buses);
			
    /* spit out something meaningful */
    usbi_debug(NULL, 3, "found bus %s", ibus->sys_path);
			
    /* release the device */
    (*(device))->Release(device);
  }

  IOObjectRelease(deviceIterator);

  return (OPENUSB_SUCCESS);
}


static int32_t process_new_device (struct usbi_bus *ibus, usb_device_t **device, UInt32 locationID) {
  UInt16                address, idVendor, idProduct;
  UInt8                 bDeviceClass, bDeviceSubClass;
  struct usbi_device   *idev;

  (*(device))->GetDeviceAddress(device, (USBDeviceAddress *)&address);
  (*(device))->GetDeviceClass (device, &bDeviceClass);
  (*(device))->GetDeviceSubClass (device, &bDeviceSubClass);
  (*(device))->GetDeviceProduct (device, &idProduct);
  (*(device))->GetDeviceVendor (device, &idVendor);
  
  idev = calloc (1, sizeof(struct usbi_device));
  if (!idev) {
    usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_refresh_devices: out of Memory (usbi_device)");
    return OPENUSB_NO_RESOURCES;
  }

  idev->priv = calloc (1, sizeof(struct usbi_dev_private));
  if (!idev->priv) {
    usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_refresh_devices: out of Memory (usbi_dev_private)");
    return OPENUSB_NO_RESOURCES;
  }

  idev->bus = ibus;
  idev->devnum = address;

  /* Save our location, we'll need this later */
  idev->priv->location = locationID;
  sprintf(idev->sys_path, "%03i-%04x-%04x-%02x-%02x", address, idVendor, idProduct, bDeviceClass, bDeviceSubClass);

  /* Add our device */
  usbi_add_device(ibus, idev);

  /* spit out something useful */
  usbi_debug(NULL, 2, "Found %d on %s", idev->devnum, ibus->sys_path);
  
  return 0;
}


/*
 * darwin_refresh_devices
 *
 *  Make a new search of the devices on the bus and refresh the device list.
 *  The device nodes that have been detached from the system would be removed 
 *  from the list.
 */
int32_t darwin_refresh_devices(struct usbi_bus *ibus)  {
  io_iterator_t        deviceIterator;
  usb_device_t         **device;
  kern_return_t        kresult;
  UInt16               idProduct;
  UInt32               location;
  IOUSBDevRequest      req;
  usb_device_desc_t    desc;
  uint32_t             count;

  /* Validate parameters and the master port */
  if (!ibus || !openusb_darwin_mp)
    return OPENUSB_BADARG;
	
  kresult = usb_setup_device_iterator (&deviceIterator);
  if (kresult)
    return darwin_to_openusb (kresult);

  /* Set up request for device descriptor */
  req.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
  req.bRequest = kUSBRqGetDescriptor;
  req.wValue = kUSBDeviceDesc << 8;
  req.wIndex = 0;
  req.wLength = sizeof(IOUSBDeviceDescriptor);
  req.pData   = calloc (1, sizeof (IOUSBDeviceDescriptor));
  if (!req.pData) {
    usbi_debug (NULL, 1,
		"libopenusb/darwin.c darwin_refresh_devices(calloc): could not allocate"
		"memory for descriptor");
    return OPENUSB_PLATFORM_FAILURE;
  }

  while ((device = usb_get_next_device (deviceIterator, &location)) != NULL) {
    if ((location >> 24) == ibus->busnum) {
      (*(device))->GetDeviceProduct (device, &idProduct);

      kresult = (*(device))->DeviceRequest(device, &req);
      if (kresult) {
	usbi_debug (NULL, 1, "libopenusb/darwin.c darwin_refresh_devices(DeviceRequest): %s",
		    darwin_error_str (kresult));

	(*(device))->Release(device);
      }

      openusb_parse_data ("bbwbbbbwwwbbbb", req.pData, USBI_DEVICE_DESC_SIZE, &desc,
			  sizeof (usb_device_desc_t), &count);

      /*
	Catch buggy hubs (which appear to be virtual). Apple's own USB prober has problems
	with these devices.
      */
      if (desc.idProduct != idProduct) {
	/* mot an error, just skip it */
	kresult = 0;
	usbi_debug (NULL, 2,
		    "libopenusb/darwin.c darwin_refresh_devices: idProduct from iokit does not"
		    " match idProduct in descriptor. Skipping device");
      } else
	kresult = process_new_device (ibus, device, location);
    }

    (*(device))->Release(device);

    if (kresult) {
      free (req.pData);
      IOObjectRelease(deviceIterator);

      return kresult;
    }
  }

  free (req.pData);

  IOObjectRelease(deviceIterator);

  return OPENUSB_SUCCESS;
}



/*
 * darwin_free_device
 *
 *  Cleanup backend specific data in the usbi_device structure. Called when the
 *  device node is to be removed from the device list.
 */
void darwin_free_device(struct usbi_device *idev) {
  /* Free the private data structure */
  if (idev && idev->priv) {
    free(idev->priv);
    idev->priv = NULL; 
  }
}


static kern_return_t darwin_get_device (uint32_t dev_location, usb_device_t ***darwin_device) {
  kern_return_t kresult;
  UInt32        location;
  io_iterator_t deviceIterator;

  kresult = usb_setup_device_iterator (&deviceIterator);
  if (kresult)
    return kresult;

  /* This port of libusb uses locations to keep track of devices. */
  while ((*darwin_device = usb_get_next_device (deviceIterator, &location)) != NULL) {
    if (location == dev_location)
      break;

    (**darwin_device)->Release(*darwin_device);
  }

  IOObjectRelease(deviceIterator);

  if (!(*darwin_device))
    return kIOReturnNoDevice;

  return kIOReturnSuccess;
}

/*
 * darwin_open
 *
 *   Prepare the device and make the default endpoint accessible
 */
int32_t darwin_open (struct usbi_dev_handle *hdev) {
  kern_return_t          kresult;
  usb_device_t           **darwin_device;

  /* Validate... */
  if (!hdev || !openusb_darwin_mp)
    return OPENUSB_BADARG;

  usbi_debug (hdev->lib_hdl, 4, "libopenusb/darwin.c darwin_open: entering..");

  /* allocate memory for our private data */
  hdev->priv = calloc(1, sizeof(struct usbi_dev_hdl_private));
  if (!hdev->priv) {
    usbi_debug(hdev->lib_hdl, 1, "libopenusb/darwin.c darwin_open: out of memory (dev_handle_private)");
    return (OPENUSB_NO_RESOURCES);
  }

  kresult = darwin_get_device (hdev->idev->priv->location, &darwin_device);
  if (kresult) {
    usbi_debug (hdev->lib_hdl, 1, "libopenusb/darwin.c darwin_open: could not find device: %s",
		darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  hdev->priv->device = darwin_device;

  /* Try to actually open the device */
#if !defined (OPENUSB_NO_SEIZE_DEVICE)
  kresult = (*(hdev->priv->device))->USBDeviceOpenSeize (hdev->priv->device);
#else
  /* No Seize in OS X 10.0 (Darwin 1.4) */
  kresult = (*(hdev->priv->device))->USBDeviceOpen (hdev->priv->device);
#endif

  if (kresult != kIOReturnSuccess) {
    switch (kresult) {
    case kIOReturnExclusiveAccess:
      usbi_debug(hdev->lib_hdl, 3,
		 "libopenusb/darwin.c darwin_open(USBDeviceOpen): %s",
		 darwin_error_str(kresult));
      break;
    default:
      (*(hdev->priv->device))->Release (hdev->priv->device);
      free (hdev->priv);
      hdev->priv = NULL;
      usbi_debug(hdev->lib_hdl, 1,
		 "libopenusb/darwin.c darwin_open(USBDeviceOpen): %s",
		 darwin_error_str(kresult));
      return darwin_to_openusb (kresult);
    }

    /* It is possible to perform some actions on a device that is not open */
    hdev->priv->open = 0;
  } else {
    hdev->priv->open = 1;

    /* create async event source */
    kresult = (*(hdev->priv->device))->CreateDeviceAsyncEventSource (hdev->priv->device, &hdev->priv->cfSource);

    CFRetain (openusb_darwin_acfl);

    /* add the cfSource to the aync run loop */
    CFRunLoopAddSource(openusb_darwin_acfl, hdev->priv->cfSource, kCFRunLoopDefaultMode);
  }

  usbi_debug (hdev->lib_hdl, 4, "libopenusb/darwin.c darwin_open: complete");

  return OPENUSB_SUCCESS;
}



/*
 * darwin_close
 *
 *  Close the device and return it to it's original state
 */
int32_t darwin_close(struct usbi_dev_handle *hdev) {
  kern_return_t	kresult;
  int i;

  /* Validate... */
  if (!hdev)
    return OPENUSB_BADARG;

  pthread_mutex_lock(&hdev->lock);
  hdev->state = USBI_DEVICE_CLOSING;
  pthread_mutex_unlock(&hdev->lock);

  /* make sure the interfaces are released */
  for (i = 0 ; i < USBI_MAXINTERFACES ; i++)
    if (hdev->claimed_ifs[i].clm == USBI_IFC_CLAIMED)
      darwin_release_interface (hdev, i);

  pthread_mutex_lock(&hdev->lock);
  if (hdev->priv->open) {
    /* delete the device's async event source */
    CFRunLoopRemoveSource (openusb_darwin_acfl, hdev->priv->cfSource, kCFRunLoopDefaultMode);
    CFRelease (hdev->priv->cfSource);

    /* close the device */
    kresult = (*(hdev->priv->device))->USBDeviceClose(hdev->priv->device);
    if (kresult) {
      /* Log the fact that we had a problem closing the file, however failing a
       * close isn't really an error, so return success anyway */
      usbi_debug(hdev->lib_hdl, 2,
		 "libopenusb/darwin.c darwin_close(USBDeviceClose) (location: %d): %s",
		 hdev->idev->priv->location, darwin_error_str(kresult));
    }
  }
	
  kresult = (*(hdev->priv->device))->Release(hdev->priv->device);
  if (kresult) {
    /* Log the fact that we had a problem closing the file, however failing a
     * close isn't really an error, so return success anyway */
    usbi_debug(hdev->lib_hdl, 2,
	       "libopenusb/darwin.c darwin_close(Release) (location: %d): %s",
	       hdev->idev->priv->location, darwin_error_str(kresult));
  }
  pthread_mutex_unlock(&hdev->lock);

  /* free our private data */
  free(hdev->priv);

  return OPENUSB_SUCCESS;
} 



/*
 * darwin_set_configuration
 *
 *  Sets the usb configuration, via IOCTL_USB_SETCONFIG
 */
int32_t darwin_set_configuration(struct usbi_dev_handle *hdev, uint8_t cfg) {
  kern_return_t kresult;
  int i, claimed_ifs[USBI_MAXINTERFACES];

  /* Validate... */
  if (!hdev || !hdev->priv || !hdev->priv->device)
    return OPENUSB_BADARG;

  usbi_debug (hdev->lib_hdl, 4, "libopenusb/darwin.c darwin_set_configuration: entering...");

  /* Setting configuration will invalidate the interface, so we need
     to reclaim it. First, dispose of existing interface, if any. */
  for (i = 0 ; i < USBI_MAXINTERFACES ; i++) {
    if (hdev->claimed_ifs[i].clm == USBI_IFC_CLAIMED) {
      darwin_release_interface (hdev, i);
      claimed_ifs[i] = 1;
    } else
      claimed_ifs[i] = 0;
  }

  kresult = (*(hdev->priv->device))->SetConfiguration(hdev->priv->device, cfg);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_set_configuration(SetConfiguration) (configuration: %d): %s",
	       cfg, darwin_error_str(kresult));
    return darwin_to_openusb(kresult);
  }

  hdev->idev->cur_config = cfg;
  hdev->config_value = cfg;

  /* Reclaim interfaces. */
  for (i = 0 ; i < USBI_MAXINTERFACES ; i++) {
    if (claimed_ifs[i] == 1)
      darwin_claim_interface (hdev, i, 0);
  }
	
  usbi_debug (hdev->lib_hdl, 4, "libopenusb/darwin.c darwin_set_configuration: complete");

  return OPENUSB_SUCCESS;
}

int32_t darwin_get_interface (usb_device_t **darwin_device, uint8_t ifc, io_service_t *usbInterfacep) {
  IOUSBFindInterfaceRequest request;
  uint8_t                   current_interface;
  kern_return_t             kresult;
  io_iterator_t             interface_iterator;

  *usbInterfacep = IO_OBJECT_NULL;
	
  /* Setup the Interface Request */
  request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
  request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
  request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
  request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

  kresult = (*(darwin_device))->CreateInterfaceIterator(darwin_device, &request, &interface_iterator);
  if (kresult)
    return kresult;

  for ( current_interface = 0 ; current_interface <= ifc ; current_interface++ )
    *usbInterfacep = IOIteratorNext(interface_iterator);
		
  /* done with the interface iterator */
  IOObjectRelease(interface_iterator);
	
  return 0;
}

static int get_endpoints (struct usbi_dev_handle *hdev, int ifc) {
  kern_return_t kresult;

  u_int8_t numep, direction, number;
  u_int8_t dont_care1, dont_care3;
  u_int16_t dont_care2;
  int i;

  if (!hdev || !hdev->priv || !hdev->priv->interfaces[ifc].interface)
    return EINVAL;

  usbi_debug(hdev->lib_hdl, 4, "libopenusb/darwin.c get_endpoints: building table of endpoints.");
  
  /* retrieve the total number of endpoints on this interface */
  kresult = (*(hdev->priv->interfaces[ifc].interface))->GetNumEndpoints(hdev->priv->interfaces[ifc].interface, &numep);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1, "get_endpoints: can't get number of endpoints for interface: %s",
	       darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }

  /* iterate through pipe references */
  for (i = 1 ; i <= numep ; i++) {
    kresult = (*(hdev->priv->interfaces[ifc].interface))->GetPipeProperties(hdev->priv->interfaces[ifc].interface,
									     i, &direction, &number, &dont_care1,
									     &dont_care2, &dont_care3);

    if (kresult != kIOReturnSuccess) {
      usbi_debug(hdev->lib_hdl, 1,
		 "libopenusb/darwin.c get_endpoints: an error occurred getting pipe information on pipe %d: %s",
		 i, darwin_error_str(kresult));
      return darwin_to_openusb (kresult);
    }

    usbi_debug(hdev->lib_hdl, 4, "libopenusb/darwin.c get_endpoints: Interface: %i Pipe %i: DIR: %i number: %i",
	       ifc, i, direction, number);

    hdev->priv->interfaces[ifc].endpoint_addrs[i - 1] = ((direction << 7 & USB_ENDPOINT_DIR_MASK) |
					      (number & USB_ENDPOINT_NUM_MASK));
  }

  hdev->priv->interfaces[ifc].num_endpoints = numep;

  usbi_debug(hdev->lib_hdl, 4, "libopenusb/darwin.c get_endpoints: complete.");
  
  return OPENUSB_SUCCESS;
}


/*
 * darwin_claim_interface
 *
 *  Claims the USB Interface, via IOCTL_USB_CLAIMINTF
 */
int32_t darwin_claim_interface (struct usbi_dev_handle *hdev, uint8_t ifc, openusb_init_flag_t flags) {
  io_service_t          usbInterface = IO_OBJECT_NULL;
  kern_return_t         kresult;
  IOCFPlugInInterface **plugInInterface = NULL;
  long                  score;
  
  /* Validate... */
  if (!hdev)
    return (OPENUSB_BADARG);

  usbi_debug(hdev->lib_hdl, 4, "libopenusb/darwin.c darwin_claim_interface: claiming interface %d", ifc);

  kresult = darwin_get_interface (hdev->priv->device, ifc, &usbInterface);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1, "claim_interface(darwin_get_interface): %s", darwin_error_str(kresult));
    return darwin_to_openusb(kresult);
  }

  /* make sure we have an interface */
  if (!usbInterface && hdev->priv->open && hdev->config_value == 0) {
    u_int8_t nConfig;			     /* Index of configuration to use */
    IOUSBConfigurationDescriptorPtr configDesc; /* to describe which configuration to select */
    /* Only a composite class device with no vendor-specific driver will
       be configured. Otherwise, we need to do it ourselves, or there
       will be no interfaces for the device. */

    usbi_debug(hdev->lib_hdl, 3,
	       "libopenusb/darwin.c darwin_claim_interface: no interface found; selecting configuration" );

    kresult = (*(hdev->priv->device))->GetNumberOfConfigurations ( hdev->priv->device, &nConfig );
    if (kresult != kIOReturnSuccess) {
      usbi_debug(hdev->lib_hdl, 1, "claim_interface(GetNumberOfConfigurations): %s",
		 darwin_error_str(kresult));
      return darwin_to_openusb(kresult);
    }
		
    if (nConfig < 1) {
      usbi_debug(hdev->lib_hdl, 1 ,"libopenusb/darwin.c"
		 " darwin_claim_interface(GetNumberOfConfigurations): no configurations");
      return OPENUSB_PLATFORM_FAILURE;
    }

    usbi_debug(hdev->lib_hdl, 3,
	       "libopenusb/darwin.c darwin_claim_interface: device has more than one configuration,"
	       " using the first (warning)");
    usbi_debug(hdev->lib_hdl, 3,
	       "libopenusb/darwin.c darwin_claim_interface: device has %d configuration%s",
	       (int)nConfig, (nConfig > 1 ? "s" : "") );

    /* Always use the first configuration */
    kresult = (*(hdev->priv->device))->GetConfigurationDescriptorPtr (hdev->priv->device, 0, &configDesc);
    if (kresult != kIOReturnSuccess)
      usbi_debug(hdev->lib_hdl, 1, "libopenusb/darwin.c darwin_claim_interface(GetConfigurationDescriptorPtr): %s",
		 darwin_error_str(kresult));

    usbi_debug(hdev->lib_hdl, 3, "libopenusb/darwin.c darwin_claim_interface: configuration value is %d",
	       configDesc->bConfigurationValue );

    /* set the configuration */
    kresult = darwin_set_configuration (hdev, configDesc->bConfigurationValue);
    if (kresult != OPENUSB_SUCCESS) {
      usbi_debug(hdev->lib_hdl, 1, "libopenusb/darwin.c darwin_claim_interface: could not set configuration");	
      return kresult;
    }

    kresult = darwin_get_interface (hdev->priv->device, ifc, &usbInterface);
    if (kresult) {
      usbi_debug(hdev->lib_hdl, 1,
		 "libopenusb/darwin.c darwin_claim_interface(darwin_get_interface): %s",
		 darwin_error_str(kresult));
      return darwin_to_openusb (kresult);
    }
  }

  if (!usbInterface) {
    usbi_debug(hdev->lib_hdl, 1, "libopenusb/darwin.c darwin_claim_interface: no such interface");
    return OPENUSB_INVALID_HANDLE;
  }
  
  /* get an interface to the device's interface */
  kresult = IOCreatePlugInInterfaceForService (usbInterface, kIOUSBInterfaceUserClientTypeID,
					       kIOCFPlugInInterfaceID, &plugInInterface, &score);
  kresult = IOObjectRelease(usbInterface);
  if (kresult || !plugInInterface)
    return OPENUSB_PLATFORM_FAILURE;
	
  /* Do the actual claim */
  kresult = (*plugInInterface)->QueryInterface(plugInInterface,
					       CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
					       (LPVOID)&hdev->priv->interfaces[ifc].interface);
  if (kresult || !hdev->priv->interfaces[ifc].interface) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_claim_interface(QueryInterface): %s",
	       darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }
	
  /* We no longer need the intermediate plug-in */
  (*plugInInterface)->Release(plugInInterface);

  /* claim the interface */
  kresult = (*(hdev->priv->interfaces[ifc].interface))->USBInterfaceOpen(hdev->priv->interfaces[ifc].interface);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_claim_interface(USBInterfaceOpen): %s",
	       darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }
		
  /* keep track of the fact that this interface was claimed */
  hdev->claimed_ifs[ifc].clm = USBI_IFC_CLAIMED;
  hdev->claimed_ifs[ifc].altsetting = 0;

  /* update list of endpoints */
  kresult = get_endpoints (hdev, ifc);

  if (kresult) {
    /* this should not happen */
    darwin_release_interface (hdev, ifc);
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_claim_interface: could not build endpoint table");
    return kresult;
  }

  /* create async event source */
  kresult = (*(hdev->priv->interfaces[ifc].interface))->CreateInterfaceAsyncEventSource (hdev->priv->interfaces[ifc].interface,
											  &hdev->priv->interfaces[ifc].cfSource);
  
  /* add the cfSource to the aync run loop */
  CFRetain (openusb_darwin_acfl);	
  CFRunLoopAddSource(openusb_darwin_acfl, hdev->priv->interfaces[ifc].cfSource, kCFRunLoopDefaultMode);

  /* There's no usbfs IOCTL for querying the current alternate setting, we'll
   * leave it up to the user to set later. */
  return OPENUSB_SUCCESS;
}



/*
 * darwin_release_interface
 * 
 *  Releases the specified interface
 */
int32_t darwin_release_interface (struct usbi_dev_handle *hdev, uint8_t ifc) {
  kern_return_t kresult;

  /* Validate... */
  if (!hdev || !hdev->priv)
    return OPENUSB_BADARG;

  /* Check to see if an interface is open */
  if (!hdev->priv->interfaces[ifc].interface)
    return (OPENUSB_SUCCESS);

  /* clean up endpoint data */
  hdev->priv->interfaces[ifc].num_endpoints = 0;
  
  /* keep track of the fact that this interface was released */
  hdev->claimed_ifs[ifc].clm = -1;
  hdev->claimed_ifs[ifc].altsetting = -1;
	
  /* delete the interface's async event source */
  CFRunLoopRemoveSource (openusb_darwin_acfl, hdev->priv->interfaces[ifc].cfSource, kCFRunLoopDefaultMode);
  CFRelease (hdev->priv->interfaces[ifc].cfSource);

  kresult = (*(hdev->priv->interfaces[ifc].interface))->USBInterfaceClose(hdev->priv->interfaces[ifc].interface);
  if (kresult)
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_release_interface(USBInterfaceClose): %s",
	       darwin_error_str(kresult));

  kresult = (*(hdev->priv->interfaces[ifc].interface))->Release(hdev->priv->interfaces[ifc].interface);
  if (kresult != kIOReturnSuccess)
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_release_interface(Release): %s",
	       darwin_error_str(kresult));

  hdev->priv->interfaces[ifc].interface = IO_OBJECT_NULL;

  return darwin_to_openusb (kresult);
}



/*
 * darwin_set_altsetting
 *
 *  Sets the alternate setting, via IOCTL_USB_SETINTF
 */
int32_t darwin_set_altsetting (struct usbi_dev_handle *hdev, uint8_t ifc, uint8_t alt) {
  kern_return_t kresult;

  /* Validate... */
  if (!hdev)
    return OPENUSB_BADARG;

  if (hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED) {
    usbi_debug(hdev->lib_hdl, 1,
	       "interface (%d) must be claimed before assigning an alternate setting",
	       ifc);
    return OPENUSB_BADARG;
  }
  
  kresult = (*(hdev->priv->interfaces[ifc].interface))->SetAlternateInterface(hdev->priv->interfaces[ifc].interface,
									       alt);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1, 
	       "libopenusb/darwin.c darwin_set_altsetting(SetAlternateInterface) (ifc = %d, alt = %d): %s",
	       ifc, alt, darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }

  /* keep track of the alternate setting */
  hdev->claimed_ifs[ifc].altsetting = alt;
  
  return OPENUSB_SUCCESS;
}


/******************************************************************************
 *                                IO Functions                                *
 *****************************************************************************/

static void darwin_io_callback (void *refcon, kern_return_t result, void *io_size) {
  struct usbi_io *io = (struct usbi_io *)refcon;
  int32_t status;

  usbi_debug(io->dev->lib_hdl, 4, "libopenusb/darwin.c darwin_io_callback: io async operation"
	     " completed: %s, size=%lu, result=0x%08x", darwin_error_str(result),
	     (uint32_t)io_size, result);

  switch (result) {
  case kIOReturnSuccess:
    status = OPENUSB_SUCCESS;
    io->status = USBI_IO_COMPLETED;
    break;
  case kIOReturnAborted:
    status = OPENUSB_IO_CANCELED;
    io->status = USBI_IO_CANCEL;
    break;
  case kIOReturnTimeout:
    status = OPENUSB_IO_TIMEOUT;
    io->status = USBI_IO_TIMEOUT;
    break;
  default:
    status = OPENUSB_SYS_FUNC_FAILURE;
    io->status = USBI_IO_COMPLETED_FAIL;
  }
	
  usbi_io_complete (io, status, (int32_t)io_size);
  if (io->priv)
    free (io->priv);
}

/*
 * ep_to_pipeRef
 *
 *	Determine what pipeRef is associated with an endpoint
 */
int32_t ep_to_pipeRef(struct usbi_dev_handle *hdev, uint8_t ep, uint8_t *pipep, uint8_t *ifcp) {
  int8_t i, ifc;

  usbi_debug(hdev->lib_hdl, 4, "libopenusb/darwin.c ep_to_pipeRef: Converting ep address to pipeRef.");

  for (ifc = 0 ; ifc < USBI_MAXINTERFACES ; ifc++)
    if (hdev->claimed_ifs[ifc].clm == USBI_IFC_CLAIMED) {
      for (i = 0 ; i < hdev->priv->interfaces[ifc].num_endpoints ; i++)
	if (hdev->priv->interfaces[ifc].endpoint_addrs[i] == ep) {
	  *pipep = i + 1;
	  *ifcp = ifc;
	  usbi_debug(hdev->lib_hdl, 4,
		     "libopenusb/darwin.c ep_to_pipeRef: pipe %d on interface %d has endpoint"
		     " address 0x%02x.", *pipep, *ifcp, ep);
	  return 0;
	}
    }
  
  /* No pipe found with the correct endpoint address */
  usbi_debug(hdev->lib_hdl, 1, "libopenusb/darwin.c ep_to_pipeRef: No pipeRef found with endpoint address"
	     " 0x%02x.", ep);
  
  return -1;
}



/*
 * darwin_submit_ctrl_sync
 *
 *	Submit a synchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_ctrl_sync (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_ctrl(hdev, io, PATTERN_SYNC);
}



/*
 * darwin_submit_ctrl_async
 *
 *	Submit an asynchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_ctrl_async (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_ctrl(hdev, io, PATTERN_ASYNC);
}



/*
 * darwin_submit_bulk_sync
 *
 *	Submit a synchronous io request to the bulk endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_bulk_sync (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_bulk_intr(hdev, io, PATTERN_SYNC);
}



/*
 * darwin_submit_bulk_async
 *
 *	Submit an asynchronous io request to the bulk endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_bulk_async (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_bulk_intr(hdev, io, PATTERN_ASYNC);
}



/*
 * darwin_submit_intr_sync
 *
 *	Submit a synchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_intr_sync (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_bulk_intr(hdev, io, PATTERN_SYNC);
}



/*
 * darwin_submit_intr_async
 *
 *	Submit an asynchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_intr_async (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_bulk_intr(hdev, io, PATTERN_ASYNC);
}


/*
 * darwin_submit_isoc_sync
 *
 *	Submit a synchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_isoc_sync (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_isoc(hdev, io, PATTERN_SYNC);
}


/*
 * darwin_submit_isoc_async
 *
 *	Submit a synchronous io request to the control endpoint (this is just a
 *	place holder for use in usbi_backend_ops, it calls darwin_submit_ctrl to do
 *	all the real work).
 */
int32_t darwin_submit_isoc_async (struct usbi_dev_handle *hdev, struct usbi_io *io) {
  return darwin_submit_isoc(hdev, io, PATTERN_ASYNC);
}



/*
 * darwin_submit_ctrl
 *
 *  Submits an io request to the control endpoint
 */
int32_t darwin_submit_ctrl (struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern) {
  openusb_ctrl_request_t *ctrl;
  IOReturn               ret;
  
#if !defined (OPENUSB_NO_TIMEOUT_DEVICE)
  IOUSBDevRequestTO      req;
#else
  IOUSBDevRequest        req;
#endif

  /* Validate... */
  if (!hdev || !io)
    return OPENUSB_BADARG;

  /* lock the io while we set things up */
  pthread_mutex_lock(&io->lock);

  /* get a pointer to the request */
  ctrl = io->req->req.ctrl;

  bzero(&req, sizeof(req));

  req.bmRequestType		= ctrl->setup.bmRequestType;
  req.bRequest			= ctrl->setup.bRequest;
  req.wValue				= ctrl->setup.wValue;
  req.wIndex				= ctrl->setup.wIndex;
  req.wLength				= ctrl->length;
  req.pData				= ctrl->payload;
#if !defined (LIBUSB_NO_TIMEOUT_DEVICE)
  req.completionTimeout	= ctrl->timeout;
  req.noDataTimeout		= ctrl->timeout;
#endif

  /* lock the device */
  pthread_mutex_lock(&hdev->lock);

  if (pattern == PATTERN_ASYNC) {
    io->priv = calloc (1, sizeof (struct usbi_io_private));
    memmove (&(io->priv->req), &req, sizeof (req));

#if !defined (LIBUSB_NO_TIMEOUT_DEVICE)
    ret = (*(hdev->priv->device))->DeviceRequestAsyncTO(hdev->priv->device, &(io->priv->req),
							 darwin_io_callback, io);
#else
    ret = (*(hdev->priv->device))->DeviceRequestAsync(hdev->priv->device, &(io->priv->req),
						       darwin_io_callback, io);
#endif
  } else {
#if !defined (LIBUSB_NO_TIMEOUT_DEVICE)
    ret = (*(hdev->priv->device))->DeviceRequestTO(hdev->priv->device, &req);
#else
    ret = (*(hdev->priv->device))->DeviceRequest(hdev->priv->device, &req);
#endif
  }

  /* unlock the device & io request */
  pthread_mutex_unlock(&io->lock);
  pthread_mutex_unlock(&hdev->lock);

  if (ret) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_submit_ctrl: Failed to submit control request: %s",
	       darwin_error_str(ret));

    if (io->priv)
      free (io->priv);

    return darwin_to_openusb (ret);
  }

  /* it may not be possible to determine how many bytes were transferred */
  ctrl->result.transferred_bytes = req.wLength;
		
  return (OPENUSB_SUCCESS);
}



/*
 * darwin_submit_bulk_intr
 *
 *  Submits an io request to a bulk endpoint
 */
int32_t darwin_submit_bulk_intr (struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern) {
  IOReturn               ret;
  uint8_t                read; /* 0 = we're reading, 1 = we're writing */
  uint8_t                *payload;
  uint32_t               length;
  uint8_t                transferType;
  /* None of the values below are used in libopenusb for bulk transfers */
  uint8_t                direction, number, interval, pipeRef, ifc;
  uint16_t               maxPacketSize;
  	
  /* Validate... */
  if (!hdev || !io)
    return OPENUSB_BADARG;

  /* lock the io while we set things up */
  pthread_mutex_lock(&io->lock);

  if (io->req->type == USB_TYPE_BULK) {
    payload = io->req->req.bulk->payload;
    length = io->req->req.bulk->length;
  } else {
    payload = io->req->req.intr->payload;
    length = io->req->req.intr->length;
  }		

  /* are we reading or writing? */
  read = io->req->endpoint & USB_REQ_DEV_TO_HOST;
	
  /* lock the device */
  pthread_mutex_lock(&hdev->lock);
	
  ep_to_pipeRef (hdev, io->req->endpoint, &pipeRef, &ifc);

  (*(hdev->priv->interfaces[ifc].interface))->GetPipeProperties (hdev->priv->interfaces[ifc].interface,
								  pipeRef, &direction, &number,
								  &transferType, &maxPacketSize, &interval);
	
#if !defined (LIBUSB_NO_TIMEOUT_DEVICE)
  /* timeouts are unavailable on interrupt endpoints */
  if (transferType == kUSBInterrupt)
#endif
    {
      /* submit the request */
      if (pattern == PATTERN_ASYNC) {
	/* submit the request asynchronously */

	if (read) {
	  ret = (*(hdev->priv->interfaces[ifc].interface))->ReadPipeAsync(hdev->priv->interfaces[ifc].interface,
								pipeRef, payload, length,
								darwin_io_callback, io);
	} else {
	  ret = (*(hdev->priv->interfaces[ifc].interface))->WritePipeAsync(hdev->priv->interfaces[ifc].interface,
								 pipeRef, payload, length,
								 darwin_io_callback, io);
	}
      } else {
	/* submit the request synchronously */
	if (read) {
	  ret = (*(hdev->priv->interfaces[ifc].interface))->ReadPipe(hdev->priv->interfaces[ifc].interface,
							   pipeRef, payload, (UInt32 *)&length);
	} else {
	  ret = (*(hdev->priv->interfaces[ifc].interface))->WritePipe(hdev->priv->interfaces[ifc].interface,
							    pipeRef, payload, length);
	}
      }
    }
#if !defined (LIBUSB_NO_TIMEOUT_DEVICE)
  else {
    /* submit the request */
    if (pattern == PATTERN_ASYNC) {
      /* submit the request asynchronously */

      if (read) {
	ret = (*(hdev->priv->interfaces[ifc].interface))->ReadPipeAsyncTO(hdev->priv->interfaces[ifc].interface,
								pipeRef, payload, length,
								io->timeout, io->timeout,
								darwin_io_callback, (void *)io);
      } else {
	ret = (*(hdev->priv->interfaces[ifc].interface))->WritePipeAsyncTO(hdev->priv->interfaces[ifc].interface,
								 pipeRef, payload, length,
								 io->timeout, io->timeout,
								 darwin_io_callback, (void *)io);
      }
    } else {
      /* submit the request synchronously */
      if (read) {
	ret = (*(hdev->priv->interfaces[ifc].interface))->ReadPipeTO(hdev->priv->interfaces[ifc].interface,
							   pipeRef, payload, (UInt32 *)&length,
							   io->timeout, io->timeout);
      } else {
	ret = (*(hdev->priv->interfaces[ifc].interface))->WritePipeTO(hdev->priv->interfaces[ifc].interface,
							    pipeRef, payload, length,
							    io->timeout, io->timeout);
      }
			
      /* if the request timed out we need to resynchronize the data */
      if (ret == kIOUSBTransactionTimeout)
	darwin_clear_halt (hdev, io->req->endpoint);
    }
  }
#endif

  /* unlock the device & io request */
  pthread_mutex_unlock(&io->lock);
  pthread_mutex_unlock(&hdev->lock);

  if (ret) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_submit_bulk_intr: bulk transfer failed (dir = %s): %s",
	       read ? "In" : "Out", darwin_error_str(ret));
    return darwin_to_openusb (ret);
  }

  if (pattern == PATTERN_SYNC) {
    if (io->req->type == USB_TYPE_BULK)
      io->req->req.bulk->result.transferred_bytes = length;
    else
      io->req->req.intr->result.transferred_bytes = length;
  }		

  return OPENUSB_SUCCESS;
}



/*
 * darwin_submit_isoc
 *
 *  Submits an io request to a bulk endpoint
 */
int32_t darwin_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io, int32_t pattern) {
  openusb_isoc_request_t *isoc;
  kern_return_t           kresult;
  IOUSBIsocFrame         *framelist;
  uint8_t                 read; /* 0 = we're writing, 1 = we're reading */
  uint8_t                *buffer, *bufferpos;
  uint8_t                 pipeRef, ifc;
  uint32_t                totallen;
  UInt64                  frame;
  AbsoluteTime            atTime;
  int                     i;
	
  /* Validate... */
  if (!hdev || !io)
    return OPENUSB_BADARG;

  /* lock the io while we set things up */
  pthread_mutex_lock(&io->lock);

  /* Just for convienience */
  isoc = io->req->req.isoc;

  /* are we reading or writing? */
  read = io->req->endpoint & USB_REQ_DEV_TO_HOST;

  /* Now we have some serious setup work to make OpenUSB's isoc structure's
   * match with those used on OS X */

  /* 1. Create memory to hold the payloads */
  totallen = 0;
  for (i = 0; i < isoc->pkts.num_packets; i++)
    totallen += isoc->pkts.packets[i].length;

  buffer = (uint8_t *) calloc (totallen, sizeof(uint8_t));
  if (!buffer) {
    pthread_mutex_unlock(&io->lock);
    return OPENUSB_NO_RESOURCES;
  }
	
  /* If we're writing we need to copy our payloads into the buffer */
  if (!read) {
    bufferpos = buffer;
    for (i = 0; i < isoc->pkts.num_packets; i++) {
      memcpy(bufferpos, isoc->pkts.packets[i].payload, isoc->pkts.packets[i].length);
      bufferpos += isoc->pkts.packets[i].length;
    }
  }
	
  /* Now construct the array of IOUSBIsocFrames */
  framelist = (IOUSBIsocFrame*) calloc (isoc->pkts.num_packets, sizeof(IOUSBIsocFrame));
  if (!framelist) {
    free(buffer);
    pthread_mutex_unlock(&io->lock);
    return OPENUSB_NO_RESOURCES;
  }
	
  /* Fill in the frame list */
  for (i = 0; i < isoc->pkts.num_packets; i++)
    framelist[i].frReqCount = isoc->pkts.packets[i].length;

  /* determine the interface/endpoint to use */
  ep_to_pipeRef (hdev, io->req->endpoint, &pipeRef, &ifc);

  /* Last but not least we need the bus frame number */
  kresult = (*(hdev->priv->interfaces[ifc].interface))->GetBusFrameNumber(hdev->priv->interfaces[ifc].interface, &frame, &atTime);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1, "failed to get bus frame number: %d", kresult);
    free(buffer);
    free(framelist);
    pthread_mutex_unlock(&io->lock);
    return (OPENUSB_SYS_FUNC_FAILURE);
  }

  /* lock the device */
  pthread_mutex_lock(&hdev->lock);
	
  /* submit the request */
  if (read) {
    kresult = (*(hdev->priv->interfaces[ifc].interface))->ReadIsochPipeAsync(hdev->priv->interfaces[ifc].interface,
							       pipeRef, buffer, frame,
							       isoc->pkts.num_packets,
							       framelist,
							       darwin_io_callback,
							       io);
  } else {
    kresult = (*(hdev->priv->interfaces[ifc].interface))->ReadIsochPipeAsync(hdev->priv->interfaces[ifc].interface,
							       pipeRef, buffer, frame,
							       isoc->pkts.num_packets,
							       framelist,
							       darwin_io_callback,
							       io);
  }

  /* unlock the device & io request */
  pthread_mutex_unlock(&io->lock);
  pthread_mutex_unlock(&hdev->lock);

  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_submit_isoc: isochronous failed (dir: %s): %s", 
	       read ? "In" : "Out", darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }
	
  return OPENUSB_SUCCESS;
}



//------------------------------------------------------------------------------


/*
 * darwin_get_configuration
 *
 *  Gets the usb configuration from our the OS.
 */
int32_t darwin_get_configuration (struct usbi_dev_handle *hdev, uint8_t *cfg) {
  kern_return_t kresult;
	
  if (!hdev || !cfg)
    return OPENUSB_BADARG;

  kresult = (*(hdev->priv->device))->GetConfiguration(hdev->priv->device, cfg);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_get_configuration(GetConfiguration): %s", 
	       darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }

  return OPENUSB_SUCCESS;
}


/*
 * darwin_get_altsetting
 *
 *  Gets the alternate setting from our the OS.
 */
int32_t darwin_get_altsetting (struct usbi_dev_handle *hdev, uint8_t ifc, uint8_t *alt) {
  kern_return_t kresult;

  /* Validate... */
  if (!hdev || !alt)
    return OPENUSB_BADARG;

  if (hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED)
    return OPENUSB_PLATFORM_FAILURE;
	
  kresult = (*(hdev->priv->interfaces[ifc].interface))->GetAlternateSetting (hdev->priv->interfaces[ifc].interface,
								   alt);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_get_altsetting(GetAlternateSetting) (ifc = %d): %s", 
	       ifc, darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  return OPENUSB_SUCCESS;
}



/*
 * darwin_reset
 *
 *   Reset device
 */
int32_t darwin_reset (struct usbi_dev_handle *hdev) {
  kern_return_t kresult;

  /* Validate... */
  if (!hdev)
    return OPENUSB_BADARG;

  kresult = (*(hdev->priv->device))->ResetDevice(hdev->priv->device);
  if (kresult) {
    usbi_debug (hdev->lib_hdl, 1,
		"libopenusb/darwin.c darwin_reset(ResetDevice): %s",
		darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  return OPENUSB_SUCCESS;
}



/*
 * darwin_clear_halt
 *
 *  Clear halted endpoint, for backward compatibility with openusb 0.1
 */
int32_t darwin_clear_halt (struct usbi_dev_handle *hdev, uint8_t ept) {
  kern_return_t kresult;
  uint8_t pipeRef, ifc;

  /* Validate... */
  if (!hdev)
    return OPENUSB_BADARG;

  /* determine the interface/endpoint to use */
  ep_to_pipeRef (hdev, ept, &pipeRef, &ifc);

#if (InterfaceVersion < 190)
  kresult = (*(hdev->priv->interfaces[ifc].interface))->ClearPipeStall(hdev->priv->device, pipeRef);
#else
  /* newer versions of darwin support clearing additional bits on the device's endpoint */
  kresult = (*(hdev->priv->interfaces[ifc].interface))->ClearPipeStallBothEnds(hdev->priv->interfaces[ifc].interface,
								     pipeRef);
#endif
  if (kresult) {
    usbi_debug (hdev->lib_hdl, 1,
		"libopenusb/darwin.c darwin_clear_halt(ClearPipeStall): %s",
		darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  return OPENUSB_SUCCESS;
}

/*
 * darwin_io_cancel
 *
 * This function will abort all transfers on an endpoint.
 *
 */
int32_t darwin_io_cancel (struct usbi_io *io) {
  struct usbi_dev_handle *hdev;
  uint8_t pipeRef, ifc;
  kern_return_t kresult;

  if (!io)
    return OPENUSB_BADARG;

  hdev = io->dev;

  io->status = USBI_IO_CANCEL;

  pthread_mutex_lock(&hdev->lock);

  /* abort all pending transfers on the endpoint */
  if (io->req->type != USB_ENDPOINT_TYPE_CONTROL) {
    ep_to_pipeRef (hdev, io->req->endpoint, &pipeRef, &ifc);
		
    kresult = (*(hdev->priv->interfaces[ifc].interface))->AbortPipe (hdev->priv->interfaces[ifc].interface, pipeRef);
  } else
    kresult = (*(hdev->priv->device))->USBDeviceAbortPipeZero(hdev->priv->device);

  pthread_mutex_unlock(&hdev->lock);

  if (kresult) {
    usbi_debug (hdev->lib_hdl, 1,
		"libopenusb/darwin.c darwin_io_cancel(AbortPipe): %s",
		darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  return OPENUSB_SUCCESS;
}





static int desc_size[] = {
  0,
  sizeof (IOUSBDeviceDescriptor),
  sizeof (IOUSBConfigurationDescriptor),
  2,
  sizeof (IOUSBInterfaceDescriptor),
  sizeof (IOUSBEndpointDescriptor),
  sizeof (IOUSBDeviceQualifierDescriptor),
  sizeof (IOUSBConfigurationDescriptor),
  -1,
  sizeof (usb_otg_desc_t),
  sizeof (usb_debug_desc_t),
  sizeof (IOUSBInterfaceAssociationDescriptor)
};

/*
 * darwin_get_raw_desc
 *
 *  Get the raw descriptor specified. These are already cached by
 *  create_new_device, so we'll just copy over the data we need
 */
int32_t darwin_get_raw_desc(struct usbi_device *idev, uint8_t type,
                           uint8_t descidx, uint16_t langid,
                           uint8_t **buffer, uint16_t *buflen) {
  uint8_t        *devdescr = NULL;
  int32_t         sts = OPENUSB_SUCCESS;
  usb_device_t  **darwin_device;
  kern_return_t   kresult;
  IOUSBDevRequest req;

  /* Validate... */
  if (!idev || !buflen)
    return OPENUSB_BADARG;

  usbi_debug (NULL, 4, "libopenusb/darwin.c darwin_get_raw_desc: entering...");

  kresult = darwin_get_device (idev->priv->location, &darwin_device);
  if (kresult) {
    usbi_debug (NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: could not find device: %s",
		darwin_error_str (kresult));
    return darwin_to_openusb (kresult);
  }

  if (desc_size[type] < 0) {
    usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: unsupported descriptor type");
    return OPENUSB_BADARG;
  }

  /* common request parameters */
  req.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
  req.bRequest      = kUSBRqGetDescriptor;
  req.wIndex        = 0;

  if (type == USB_DESC_TYPE_CONFIG) {
    IOUSBConfigurationDescHeader cfg_hdr;

    /* retrieve header to determine descriptor length */
    req.wValue  = (kUSBConfDesc << 8) | descidx;
    req.wLength = sizeof (IOUSBConfigurationDescHeader);
    req.pData   = &cfg_hdr;

    kresult = (*(darwin_device))->DeviceRequest(darwin_device, &req);
    if (kresult) {
      usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: couldn't read descriptor: %s",
		 darwin_error_str(kresult));
      sts = darwin_to_openusb (kresult);
      goto done;
    }

    /* retreive entire descriptor */
    req.wLength = NXSwapLittleShortToHost (cfg_hdr.wTotalLength);
    req.pData   = calloc (1, req.wLength);
    devdescr    = req.pData;

    kresult = (*(darwin_device))->DeviceRequest(darwin_device, &req);
    if (kresult) {
      usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: couldn't read descriptor: %s",
		 darwin_error_str(kresult));
      sts = darwin_to_openusb (kresult);
    }
  } else {
    req.wValue  = (type << 8) | descidx;
    req.wLength = desc_size[type];

    devdescr = calloc(1, req.wLength);
    if (!devdescr) {
      usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: unable to allocate"
		 " memory for cached device descriptor");
      sts = OPENUSB_NO_RESOURCES;
      goto done;
    }

    /* Set up request for device descriptor */
    req.pData         = devdescr;

    kresult = (*(darwin_device))->DeviceRequest(darwin_device, &req);
    if (kresult) {
      usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: couldn't read descriptor: %s",
		 darwin_error_str(kresult));
      sts = darwin_to_openusb (kresult);
      goto done;
    }
    
    /* work around buggy devices that hang if we request 255 bytes of the string descriptor */
    if (type == USB_DESC_TYPE_STRING) {
      req.wLength = (uint8_t) devdescr[0]; /* bLength */
      devdescr = realloc (devdescr, req.wLength);
      if (!devdescr) {
	usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: unable to allocate"
		   " memory for cached device descriptor");
	sts = OPENUSB_NO_RESOURCES;
	goto done;
      }

      kresult = (*(darwin_device))->DeviceRequest(darwin_device, &req);
      if (kresult) {
	usbi_debug(NULL, 1, "libopenusb/darwin.c darwin_get_raw_desc: couldn't read descriptor: %s",
		   darwin_error_str(kresult));
	sts = darwin_to_openusb (kresult);
      }
    }
  }

  if (sts == OPENUSB_SUCCESS) {
    *buffer = devdescr;
    *buflen = req.wLength;
  }

done:

  (*(darwin_device))->Release(darwin_device);

  return sts;
}


int32_t darwin_get_driver(struct usbi_dev_handle *hdev, uint8_t interface,
			  char *name, uint32_t namelen)
{
  kern_return_t kresult;
  io_service_t usbInterface;
  CFTypeRef driver;

  if (!hdev || !name)
    return OPENUSB_BADARG;

  kresult = darwin_get_interface (hdev->priv->device, interface, &usbInterface);
  if (kresult) {
    usbi_debug(hdev->lib_hdl, 1,
	       "libopenusb/darwin.c darwin_get_driver(darwin_get_interface): %s",
	       darwin_error_str(kresult));
    return darwin_to_openusb (kresult);
  }
  
  driver = IORegistryEntryCreateCFProperty (usbInterface,
					    kIOBundleIdentifierKey,
					    kCFAllocatorDefault, 0);

  IOObjectRelease (usbInterface);


  if (driver) {
    kresult = CFStringGetCString (driver, name, namelen, kCFStringEncodingUTF8);

    CFRelease (driver);

    if (kresult) {
      usbi_debug(hdev->lib_hdl, 1,
		 "libopenusb/darwin.c darwin_get_driver(CFStringGetCString): %s",
		 darwin_error_str(kresult));
      return darwin_to_openusb (kresult);
    }
  } else
    /* no driver */
    return OPENUSB_PLATFORM_FAILURE;

  return OPENUSB_SUCCESS;
}



struct usbi_backend_ops backend_ops = {
	.backend_version				= 1,
	.io_pattern					= PATTERN_BOTH,
	.init						= darwin_init,
	.fini						= darwin_fini,
	.find_buses					= darwin_find_busses,
	.refresh_devices				= darwin_refresh_devices,
	.free_device					= darwin_free_device,
	.dev = {
		.open					= darwin_open,
		.close					= darwin_close,
		.set_configuration			= darwin_set_configuration,
		.get_configuration			= darwin_get_configuration,
		.claim_interface			= darwin_claim_interface,
		.release_interface			= darwin_release_interface,
		.get_altsetting				= darwin_get_altsetting,
		.set_altsetting				= darwin_set_altsetting,
		.reset					= darwin_reset,
		.get_driver_np				= darwin_get_driver,
		/* it is unlikely that we will be able to attach or detach kernel drivers */
		.attach_kernel_driver_np	        = NULL,
		.detach_kernel_driver_np	        = NULL,
		.ctrl_xfer_aio				= darwin_submit_ctrl_async,
		.intr_xfer_aio				= darwin_submit_intr_async,
		.bulk_xfer_aio				= darwin_submit_bulk_async,
		.isoc_xfer_aio				= darwin_submit_isoc_async,
		.ctrl_xfer_wait				= darwin_submit_ctrl_sync,
		.intr_xfer_wait				= darwin_submit_intr_sync,
		.bulk_xfer_wait				= darwin_submit_bulk_sync,
		.isoc_xfer_wait				= darwin_submit_isoc_sync,
		.io_cancel				= darwin_io_cancel,
		.get_raw_desc				= darwin_get_raw_desc,
	},
};
