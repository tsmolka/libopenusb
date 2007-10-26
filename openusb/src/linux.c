/*
 * Linux USB support
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>	/* getenv, etc */
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <dirent.h>
#include <ctype.h>
#include <stdarg.h>
#include <sysfs/libsysfs.h>

#include "usbi.h"
#include "linux.h"

static pthread_t  event_thread;
static int        resubmit_flag = RESUBMIT;

static char device_dir[PATH_MAX + 1] = "";


/*
 * translate_errno
 *
 *  Translates errno codes into the appropriate LIBUSB error code
 */
int32_t translate_errno(int errnum)
{
  switch(errnum)
  {
    default:
      return (LIBUSB_SYS_FUNC_FAILURE);

    case EPERM:
      return (LIBUSB_INVALID_PERM);

    case EINVAL: 
      return (LIBUSB_BADARG);

    case ENOMEM:
      return (LIBUSB_NO_RESOURCES);

    case EACCES:
      return (LIBUSB_NOACCESS);

    case EBUSY:
      return (LIBUSB_BUSY);

    case EPIPE:
      return (LIBUSB_IO_STALL);
  }
} 



/*
 * device_open
 *
 *  Opens the device, by opening a file descriptor for it
 */
int32_t device_open(struct usbi_device *idev)
{
  int32_t fd;

  /* Validate... */
  if (!idev)
    return LIBUSB_BADARG;  

  fd = open(idev->priv->filename, O_RDWR);
  if (fd < 0) {
    fd = open(idev->priv->filename, O_RDONLY);
    if (fd < 0) {
      usbi_debug(NULL, 1, "failed to open %s: %s", idev->priv->filename, strerror(errno));
      return translate_errno(errno);
    }
  }

  return fd;
}



/*
 * linux_open
 *
 *   Prepare the device and make the default endpoint accessible
 */
int32_t linux_open(struct usbi_dev_handle *hdev)
{
  int ret;

  /* Validate... */
  if (!hdev) {
    return (LIBUSB_BADARG);
  }

  /* allocate memory for our private data */
  hdev->priv = calloc(sizeof(struct usbi_dev_hdl_private), 1);
  if (!hdev->priv) {
    return (LIBUSB_NO_RESOURCES);
  }

  /* open the device */
  hdev->priv->fd = device_open(hdev->idev);
  if (hdev->priv->fd < 0) {
    return (hdev->priv->fd);
  }

  /* setup the event pipe for this device */ 
  pipe(hdev->priv->event_pipe);

  /* Start up thread for polling io */
  ret = pthread_create(&hdev->priv->io_thread, NULL, poll_io, (void*)hdev);
  if (ret < 0) {
    usbi_debug(NULL, 1, "unable to create io polling thread (ret = %d)", ret);
    return (LIBUSB_NO_RESOURCES);
  } 

  /* there's no need to get the current configuration here and set it, that's
   * already handled by the usbfs kernel driver. */
  return (LIBUSB_SUCCESS);
}



/*
 * linux_close
 *
 *  Close the device and return it to it's original state
 */
int32_t linux_close(struct usbi_dev_handle *hdev)
{
  struct usbi_io  *io, *tio;

  /* Validate... */
  if (!hdev)
    return LIBUSB_BADARG;

  resubmit_flag = NO_RESUBMIT;     /*stop isoc resubmit, it is a method, but not a good method. And it will be improved in near future*/

  tio = NULL;
  list_for_each_entry(io, &hdev->io_head, list) {
    pthread_mutex_lock(&io->lock);
    free(io->priv);
    usbi_free_io(tio);
    tio = io;
    pthread_mutex_unlock(&io->lock);
  }

  pthread_mutex_lock(&hdev->lock);
  hdev->state = USBI_DEVICE_CLOSING;
  pthread_mutex_unlock(&hdev->lock);

  /* Stop the IO processing (polling) thread */
  pthread_cancel(hdev->priv->io_thread);
  pthread_join(hdev->priv->io_thread, NULL);

  /* If we've already closed the file, we're done */
  if (hdev->priv->fd < 0)
    return LIBUSB_SUCCESS;

  if (close(hdev->priv->fd) == -1) {
    /* Log the fact that we had a problem closing the file, however failing a
     * close isn't really an error, so return success anyway */
    usbi_debug(hdev->lib_hdl, 2, "error closing device fd %d: %s", hdev->priv->fd, strerror(errno));
  }
  
  /* free our private data */
  free(hdev->priv);

  return (LIBUSB_SUCCESS); 
} 



/*
 * linux_set_configuration
 *
 *  Sets the usb configuration, via IOCTL_USB_SETCONFIG
 */
int32_t linux_set_configuration(struct usbi_dev_handle *hdev, uint8_t cfg)
{
  int32_t ret;

  /* Validate... */
  if (!hdev)
    return LIBUSB_BADARG;

  ret = ioctl(hdev->priv->fd, IOCTL_USB_SETCONFIG, &cfg);
  if (ret < 0) {
    usbi_debug(hdev->lib_hdl, 1, "could not set config %u: %s", cfg, strerror(errno));
    return (translate_errno(errno));
  }

  hdev->idev->cur_config = cfg;

  return (LIBUSB_SUCCESS);
}



/*
 * linux_get_configuration
 *
 *  Gets the usb configuration from our cache. There is no usbdevfs IOCTL
 *  for getting the current configuration so this is the best we can do.
 */
int32_t linux_get_configuration(struct usbi_dev_handle *hdev, uint8_t *cfg)
{
  if ((!hdev) || (!cfg))
    return LIBUSB_BADARG;

  *cfg = hdev->idev->cur_config;

  return LIBUSB_SUCCESS;
}



/*
 * linux_claim_interface
 *
 *  Claims the USB Interface, via IOCTL_USB_CLAIMINTF
 */
int32_t linux_claim_interface(struct usbi_dev_handle *hdev, uint8_t ifc,
                               libusb_init_flag_t flags)
{
  int32_t ret;

  /* Validate... */
	if (!hdev) {
		return (LIBUSB_BADARG);
	}

	usbi_debug(hdev->lib_hdl, 4, "claiming interface %d", ifc);
	
  ret = ioctl(hdev->priv->fd, IOCTL_USB_CLAIMINTF, &ifc);
  if (ret < 0) {
    usbi_debug(hdev->lib_hdl, 1, "could not claim interface %d: %s", ifc, strerror(errno));

		/* there may be another kernel driver attached to this interface, if the
		 * user requested it try and detach the kernel driver, we'll reattach
		 * it when we release the interface */
		if ((flags == USB_INIT_REVERSIBLE) || (flags = USB_INIT_NON_REVERSIBLE))
		{
			ret = linux_detach_kernel_driver(hdev,ifc);
			if (ret > 0) {
				hdev->priv->reattachdrv = 1;

				/* try to claim the interface again */
				ret = ioctl(hdev->priv->fd, IOCTL_USB_CLAIMINTF, &ifc);
				if (ret < 0) {
					hdev->priv->reattachdrv = 0;
					usbi_debug(hdev->lib_hdl, 1,
										 "could not claim interface %d, after detaching kernel driver: %s",
										 ifc, libusb_strerror(ret));
					ret = linux_attach_kernel_driver(hdev,ifc);
					if (ret < 0) {
						usbi_debug(hdev->lib_hdl, 1, "could not reattach kernel driver: %s",
											 libusb_strerror(ret));
						return (ret);
					}
				}
			} else {
				usbi_debug(hdev->lib_hdl, 1, "could not detach kernel driver: %s",
									 libusb_strerror(ret));
			}
			return (ret);
		}

		return translate_errno(errno);
  }

  /* keep track of the fact that this interface was claimed */ 
  hdev->claimed_ifs[ifc].clm = USBI_IFC_CLAIMED;
  hdev->claimed_ifs[ifc].altsetting = 0;

  /* There's no usbfs IOCTL for querying the current alternate setting, we'll
   * leave it up to the user to set later. */
  return (LIBUSB_SUCCESS);
}



/*
 * linux_release_interface
 * 
 *  Releases the specified interface, via IOCTL_USB_RELEASEINTF
 */
int32_t linux_release_interface(struct usbi_dev_handle *hdev, uint8_t ifc)
{
  int32_t ret;

  /* Validate... */
  if (!hdev) {
    return LIBUSB_BADARG;
  }  

  /* stop isoc resubmit,currently it is only a work around ,
   * but not a good solution. And it will be improved in near future,
   * maybe we need a stop_isoc() function to do it*/
  resubmit_flag = NO_RESUBMIT;

  /*wait for isoc urb to be finished, so isoc callback is better to return quickly*/
  sleep(3);

  ret = ioctl(hdev->priv->fd, IOCTL_USB_RELEASEINTF, &ifc);
  if (ret < 0) {
    usbi_debug(hdev->lib_hdl, 1, "could not release interface %d: %s", ifc, strerror(errno));
    return translate_errno(errno);
  }

  /* keep track of the fact that this interface was released */
  hdev->claimed_ifs[ifc].clm = -1;
  hdev->claimed_ifs[ifc].altsetting = -1;

	/* if necessary reattach the kernel driver */
	if (hdev->priv->reattachdrv) {
		ret = linux_attach_kernel_driver(hdev, ifc);
		if (ret < 0) {
			usbi_debug(hdev->lib_hdl, 1, "could not reattach the kernel driver");
			return (ret);
		}
	}
	
  return (LIBUSB_SUCCESS);
}



/*
 * linux_set_altsetting
 *
 *  Sets the alternate setting, via IOCTL_USB_SETINTF
 */
int32_t linux_set_altsetting(struct usbi_dev_handle *hdev, uint8_t ifc, uint8_t alt)
{
  struct usbk_setinterface setintf;
  int32_t                  ret;

  /* Validate... */
  if (!hdev)
    return LIBUSB_BADARG;

  if (hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED) {
    usbi_debug(hdev->lib_hdl, 1,
               "interface (%d) must be claimed before assigning an alternate setting",
               ifc);
    return LIBUSB_BADARG;
  }

  /* fill in the kernel structure for our IOCTL */
  setintf.interface   = ifc;
  setintf.altsetting  = alt;

  /* send the IOCTL */
  ret = ioctl(hdev->priv->fd, IOCTL_USB_SETINTF, &setintf);
  if (ret < 0) {
    usbi_debug(hdev->lib_hdl, 1,
               "could not set alternate setting for %s, ifc = %d, alt = %d: %s",
               hdev->idev->priv->filename, ifc, alt, strerror(errno));
    return (translate_errno(errno));
  }

  /* keep track of the alternate setting */
  hdev->claimed_ifs[ifc].altsetting = alt;

  return (LIBUSB_SUCCESS);
} 



/*
 * linux_get_altsetting
 *
 *  Gets the alternate setting from our cached value. There is no usbdevfs IOCTL
 *  to do this so this is the best we can do.
 */
int32_t linux_get_altsetting(struct usbi_dev_handle *hdev, uint8_t ifc, uint8_t *alt)
{
  /* Validate... */
  if ((!hdev) || (!alt))
    return LIBUSB_BADARG;

  *alt = hdev->claimed_ifs[ifc].altsetting;

  return (LIBUSB_SUCCESS);
}



/*
 * linux_reset
 *
 *   Reset device by resetting the port
 */
int32_t linux_reset(struct usbi_dev_handle *hdev)
{
  int32_t ret;

  /* Validate... */
  if (!hdev)
    return LIBUSB_BADARG;

  ret = ioctl(hdev->priv->fd, IOCTL_USB_RESET, NULL);
  if (ret) {
    usbi_debug(hdev->lib_hdl, 1, "could not reset: %s", strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}



/*
 * linux_clear_halt
 *
 *  Clear halted endpoint, for backward compatibility with libusb 0.1
 */
int32_t libusb_clear_halt(struct usbi_dev_handle *hdev, uint8_t ept)
{
  int32_t ret;

  /* Validate... */
  if (!hdev)
    return LIBUSB_BADARG;

  ret = ioctl(hdev->priv->fd, IOCTL_USB_CLEAR_HALT, &ept);
  if (ret) {
    usbi_debug(hdev->lib_hdl, 1, "could not clear halt ep %d: %s", ept, strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}



/* linux_init
 *
 *  Backend initialization, called in libusb_init()
 *    flags - inherited from libusb_init(), TBD
 */
int32_t linux_init(struct usbi_handle *hdl, uint32_t flags )
{
  int32_t ret;
  char    sysfsdir[PATH_MAX+1];

  /* Validate... */
  if (!hdl)
    return LIBUSB_BADARG;

  /* Find the path to the directory tree with the device nodes */
  if (getenv("USB_PATH")) {
    if (check_usb_path(getenv("USB_PATH"))) {
      strncpy(device_dir, getenv("USB_PATH"), sizeof(device_dir) - 1);
      device_dir[sizeof(device_dir) - 1] = 0;
    } else
      usbi_debug(hdl, 1, "couldn't find USB devices in USB_PATH");
  }

  if (!device_dir[0]) {
    if (sysfs_get_mnt_path(sysfsdir,PATH_MAX+1) == 0) {
      /* we have sysfs support so our device dir = /dev/bus/usb (from udev) */
      if (check_usb_path("/dev/bus/usb")) {
        strncpy(device_dir, "/dev/bus/usb", sizeof(device_dir) - 1);
        device_dir[sizeof(device_dir) - 1] = 0;
      } else
        device_dir[0] = 0;
    } else if (check_usb_path("/proc/bus/usb")) {
      strncpy(device_dir, "/proc/bus/usb", sizeof(device_dir) - 1);
      device_dir[sizeof(device_dir) - 1] = 0;
    } else 
      device_dir[0] = 0;  /* No path, no USB support */
  }

  if (device_dir[0])
    usbi_debug(hdl, 1, "found USB device directory at %s", device_dir);
  else
    usbi_debug(hdl, 1, "no USB device directory found");

  /* Start up thread for polling events */
  ret = 0;
  ret = pthread_create(&event_thread, NULL, poll_events, (void*)NULL);
  if (ret < 0)
    usbi_debug(hdl, 1, "unable to create event polling thread (ret = %d)", ret);

  return LIBUSB_SUCCESS;
}



/*
 * linux_fini
 *
 *  Backend specific data cleanup, called in libusb_fini()
 */
void linux_fini(struct usbi_handle *hdl)
{
  /* Stop polling for events (connect/disconnect) */
  pthread_cancel(event_thread);
  pthread_join(event_thread, NULL); 

  return;
}



/* linux_find_busses
 *
 *  Seearch USB buses under the control of the backend and return the bus list
 */
int32_t linux_find_buses(struct list_head *buses)
{
  DIR            *dir;
  struct dirent  *entry; 
  libusb_busid_t busnum;

  /* Validate... */
  if (!buses)
    return LIBUSB_BADARG;

  /* Scan our device directory for bus directories */
  dir = opendir(device_dir);
  if (!dir) {
    usbi_debug(NULL, 1, "could not opendir(%s): %s", device_dir, strerror(errno));
    return translate_errno(errno);
  }

  while ((entry = readdir(dir)) != NULL) {
    struct usbi_bus *ibus;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    if (!strchr("0123456789", entry->d_name[strlen(entry->d_name) - 1])) {
      usbi_debug(NULL, 3, "skipping non bus dir %s", entry->d_name);
      continue;
    }

    /* check our list of busses to make sure we don't have it in the list */
    busnum = atoi(entry->d_name);
    list_for_each_entry(ibus,buses,list) {
      if (ibus->busnum == busnum) {
        continue;
      }
    }

    /* FIXME: Centralize allocation and initialization */
    ibus = malloc(sizeof(*ibus));
    if (!ibus) {
      return (LIBUSB_NO_RESOURCES);
    }   
    memset(ibus, 0, sizeof(*ibus));

    ibus->priv = (struct usbi_bus_private *)
        calloc(sizeof(struct usbi_bus_private), 1);
    if (!ibus->priv) {
      free(ibus);
      usbi_debug(NULL,1, "malloc ibus private failed: %s",strerror(errno));
      return (LIBUSB_NO_RESOURCES);
    }

    /* setup the maximum transfer sizes */
    ibus->max_xfer_size[USB_TYPE_CONTROL]     = 4088;
    ibus->max_xfer_size[USB_TYPE_INTERRUPT]   = 16384;
    ibus->max_xfer_size[USB_TYPE_BULK]        = 16384;
    
    pthread_mutex_init(&ibus->lock, NULL);
    pthread_mutex_init(&ibus->devices.lock, NULL);
    
    ibus->busnum = atoi(entry->d_name);
    snprintf(ibus->priv->filename, sizeof(ibus->priv->filename), "%s/%s",
             device_dir, entry->d_name);

    list_add(&ibus->list, buses);

    usbi_debug(NULL, 3, "found bus dir %s", ibus->priv->filename);
  }

  closedir(dir);

  return LIBUSB_SUCCESS;
}



/*
 * linux_refresh_devices
 *
 *  Make a new search of the devices on the bus and refresh the device list.
 *  The device nodes that have been detached from the system would be removed 
 *  from the list.
 */
int32_t linux_refresh_devices(struct usbi_bus *ibus)
{
  char devfilename[PATH_MAX + 1];
  struct usbi_device *idev, *tidev;
  int busnum = 0, pdevnum = 0, pport = 0, devnum = 0, max_children = 0;
  char sysfsdir[PATH_MAX + 1];
  FILE *f;

  /* Validate... */
  if (!ibus) {
    return (LIBUSB_BADARG);
  }  

  /* we'll use libsysfs as our primary way of getting the device topology */
  if (sysfs_get_mnt_path(sysfsdir,PATH_MAX+1) == 0)
  {
    struct sysfs_class        *class;
    struct sysfs_class_device *classdev;
    struct sysfs_device       *dev;
    struct sysfs_device       *parentdev;
    struct sysfs_attribute    *devattr;
    struct dlist              *devlist;
    char                      *p, *b, *d, *t;

    pthread_mutex_lock(&ibus->lock);

    /* Reset the found flag for all devices */
    list_for_each_entry(idev, &ibus->devices.head, bus_list) {
        idev->found = 0;
    }

    /* if we were able to get the mount path, it's safe to say we support
    * sysfs and we'll use that to get our info */
    class = sysfs_open_class("usb_device");
    if (!class) {
      usbi_debug(NULL, 1, "could not open sysfs usb_device class: %s\n",strerror(errno));
      return translate_errno(errno);
    }
  
    devlist = sysfs_get_class_devices(class);
    if (!devlist) {
      usbi_debug(NULL, 1, "could not get sysfs/bus/usb devices: %s",strerror(errno));
      return translate_errno(errno);
    }
    
    /* loop through the list of class devices */
    dlist_for_each_data(devlist,classdev,struct sysfs_class_device) {
      
      /* the class name is usbdevBUS.DEV (where BUS is the bus number and DEV
       * is the device number, e.g usbdev8.1). We'll parse our number from 
       * this name.
       */
         
      p = classdev->name;
      while (!isdigit(*p))
        p++;
      b = p; /* bus number string starts here, save it as b */
      while (*p != '.')
        p++;
      t = p;   /* bus number string ends here, save it as t */
      d = ++p; /* device number string starts here, save it as d */ 
      *t = 0;  /* terminate our bus number string */
    
      busnum = atoi(b);
      devnum = atoi(d);
    
      /* now we'll get the sysfs_device for this class device and it's parent */
      dev = sysfs_get_classdev_device(classdev);
      if (!dev) {
        usbi_debug(NULL, 1, "could not open sysfs device this class device (%s): %s\n",classdev->name,strerror(errno));
        return translate_errno(errno);
      }
    
      /* we need to get the maximum number of children*/
      devattr = sysfs_get_device_attr(dev,"maxchild");
      if (!devattr) {
        max_children = 0;
      } else {
        max_children = atoi(devattr->value);
      }
    
      parentdev = sysfs_get_device_parent(dev);
      if (!parentdev) {
        /* we may not have a parent, in which case our parent number is 0 */
        pdevnum = 0;
      } else {
        devattr = sysfs_get_device_attr(parentdev,"devnum");
        if (!devattr) {
          /* there's no dev number for the parent, so parent num = 0 */
          pdevnum = 0;
        } else {
          pdevnum = atoi(devattr->value);
        }
      }
    
      /* now we should have all the information we need to procede */
    
      /* Is this a device on the bus we're looking for? */
      if (busnum != ibus->busnum)
        continue;

      /* Validate the data we parsed out */
      if (devnum < 1 || devnum >= USB_MAX_DEVICES_PER_BUS ||
          max_children >= USB_MAX_DEVICES_PER_BUS ||
          pdevnum >= USB_MAX_DEVICES_PER_BUS) {
        usbi_debug(NULL, 1, "invalid device number, max children or parent device");
        continue;
      }

      /* Validate the parent device */
      if (pdevnum && (!ibus->priv->dev_by_num[pdevnum])) {
        usbi_debug(NULL, 1, "no parent device or invalid child port number");
        continue;
      }

      /* make sure we don't have two root devices */
      if (!pdevnum && ibus->root && ibus->root->found) {
        usbi_debug(NULL, 1, "cannot have two root devices");
        continue;
      }

      /* Only add this device if it's new */
      /* If we don't have a device by this number yet, it must be new */
      idev = ibus->priv->dev_by_num[devnum];
      if (idev && device_is_new(idev, devnum))
        idev = NULL;

      if (!idev) {
        int ret;

        ret = create_new_device(&idev, ibus, devnum, max_children);
        if (ret) {
          usbi_debug(NULL, 1, "ignoring new device because of errors");
          continue;
        }
  
        usbi_add_device(ibus, idev);

        /* Setup parent/child relationship */
        if (pdevnum) {
          ibus->priv->dev_by_num[pdevnum]->children[pport] = idev;
          idev->parent = ibus->priv->dev_by_num[pdevnum];
        } else {
          ibus->root = idev;
        }
      }

      idev->found = 1;
    }

    /* close our sysfs class - this also destroys our dlist*/
    sysfs_close_class(class);

  } else {
    /*
     * This used to scan the bus directory for files named like devices.
     * Unfortunately, this has a couple of problems:
     * 1) Devices are added out of order. We need the root device first atleast.
     * 2) All kernels (up through 2.6.12 atleast) require write and/or root
     *    access to get to some important topology information.
     * So, we parse /proc/bus/usb/devices instead. It will give us the topology
     * information we're looking for, in the order we need, while being
     * available to normal users.
    */
     printf("start /proc/bus/usb/devices\n");
     snprintf(devfilename, sizeof(devfilename), "%s/devices", device_dir);
     f = fopen(devfilename, "r");
     if (!f) {
       usbi_debug(NULL, 1, "could not open %s: %s", devfilename, strerror(errno));
       return translate_errno(errno);
     }   
      
     pthread_mutex_lock(&ibus->lock);

     /* Reset the found flag for all devices */
     list_for_each_entry(idev, &ibus->devices.head, bus_list)
         idev->found = 0;

     while (!feof(f)) {
       char buf[1024], *p, *k, *v;

       if (!fgets(buf, sizeof(buf), f))
         continue;

       /* Strip off newlines and trailing spaces */
       for (p = strchr(buf, 0) - 1; p >= buf && isspace(*p); p--)
         *p = 0;

       /* Skip blank or short lines */
       if (!buf[0] || strlen(buf) < 4)
         continue;

       /* We need a character and colon to start the line */
       if (buf[1] != ':')
         break;

       switch (buf[0]) {
         case 'T': /* Topology information for a device. Also starts a new device */
           /* T:  Bus=02 Lev=01 Prnt=01 Port=00 Cnt=01 Dev#=  5 Spd=12  MxCh= 0 */

           /* We need the bus number, parent dev number, this dev number */

           /* Tokenize into key and value pairs */
           p = buf + 2;
           do {
             /* Skip over whitespace */
             while (*p && isspace(*p))
               p++;
             if (!*p)
               break;

             /* Parse out the key */
             k = p;
             while (*p && (isalnum(*p) || *p == '#'))
               p++;
             if (!*p)
               break;
             *p++ = 0;

             /* Skip over the = */
             while (*p && (isspace(*p) || *p == '='))
               p++;
             if (!*p)
               break;

             /* Parse out the value */
             v = p;
             while (*p && (isdigit(*p) || *p == '.'))
               p++;
             if (*p)
                *p++ = 0;

             if (strcasecmp(k, "Bus") == 0)
               busnum = atoi(v);
             else if (strcasecmp(k, "Prnt") == 0)
               pdevnum = atoi(v);
             else if (strcasecmp(k, "Port") == 0)
               pport = atoi(v);
             else if (strcasecmp(k, "Dev#") == 0)
               devnum = atoi(v);
             else if (strcasecmp(k, "MxCh") == 0)
               max_children = atoi(v);
           } while (*p);

           /* Is this a device on the bus we're looking for? */
           if (busnum != ibus->busnum)
             break;

           /* Validate the data we parsed out */
           if (devnum < 1 || devnum >= USB_MAX_DEVICES_PER_BUS ||
               max_children >= USB_MAX_DEVICES_PER_BUS ||
               pdevnum >= USB_MAX_DEVICES_PER_BUS ||
               pport >= USB_MAX_DEVICES_PER_BUS) {
             usbi_debug(NULL, 1, "invalid device number, max children or parent device");
             break;
           }

           /* Validate the parent device */
           if (pdevnum && (!ibus->priv->dev_by_num[pdevnum] ||
               pport >= ibus->priv->dev_by_num[pdevnum]->nports)) {
             usbi_debug(NULL, 1, "no parent device or invalid child port number");
             break;
           }

           if (!pdevnum && ibus->root && ibus->root->found) {
             usbi_debug(NULL, 1, "cannot have two root devices");
             break;
           }

           /* Only add this device if it's new */

           /* If we don't have a device by this number yet, it must be new */
           idev = ibus->priv->dev_by_num[devnum];
           if (idev && device_is_new(idev, devnum))
             idev = NULL;

           if (!idev) {
             int ret;

             ret = create_new_device(&idev, ibus, devnum, max_children);
             if (ret) {
               usbi_debug(NULL, 1, "ignoring new device because of errors");
               break;
             }

             usbi_add_device(ibus, idev);

             /* Setup parent/child relationship */
             if (pdevnum) {
               ibus->priv->dev_by_num[pdevnum]->children[pport] = idev;
               idev->parent = ibus->priv->dev_by_num[pdevnum];
             } else
               ibus->root = idev;
             }

             idev->found = 1;
             break;

             /* Ignore the rest */
#if 0
    case 'B': /* Bandwidth related information */
    case 'D': /* Device related information */
    case 'P': /* Vendor/Product information */
    case 'S': /* String descriptor */
    case 'C': /* Config information */
    case 'I': /* Interface information */
    case 'E': /* Endpoint information */
#endif
        default:
          break;
      }
      printf("done /proc/bus/usb/devices\n");
    }
  }

  list_for_each_entry_safe(idev, tidev, &ibus->devices.head, bus_list) {
    if (!idev->found) {
      /* Device disappeared, remove it */
      usbi_debug(NULL, 2, "device %d removed", idev->devnum);
      usbi_remove_device(idev);
    }
  }

  pthread_mutex_unlock(&ibus->lock);

  return LIBUSB_SUCCESS;
}



/*
 * linux_free_device
 *
 *  Cleanup backend specific data in the usbi_device structure. Called when the
 *  device node is to be removed from the device list.
 */
void linux_free_device(struct usbi_device *idev)
{
  free (idev->priv);
  return;
}



/******************************************************************************
 *                                IO Functions                                *
 *****************************************************************************/

/*
 * urb_submit
 *
 *  All io requests are submitted to usbfs kernel driver as a urb, this function
 *  actually submits the URB, the other functions (linux_submit_bulk -- just set
 *  up the URB).
 */
int32_t urb_submit(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  int ret;

	/* Validate ... */
	if ((!hdev) || (!io)) {
		return (LIBUSB_BADARG);
	}

	usbi_debug(hdev->lib_hdl, 4, "Submit URB");
	
	io->priv->urb.endpoint = io->req->endpoint;
  io->priv->urb.status   = 0;

  io->priv->urb.signr = 0;
  io->priv->urb.usercontext = (void *)io;

  io->status = USBI_IO_INPROGRESS;

  /* calculate the next soonest timeout */
  if (   (usbi_timeval_compare(&io->tvo, &hdev->priv->tvo) < 0)
      || (!hdev->priv->tvo.tv_sec)) {
    memcpy(&hdev->priv->tvo, &io->tvo, sizeof(hdev->priv->tvo));
  }

  /* submit this request to the usbfs kernel driver */
  ret = ioctl(hdev->priv->fd, IOCTL_USB_SUBMITURB, &io->priv->urb);
  if (ret < 0) {
    usbi_debug(hdev->lib_hdl, 1, "error submitting URB: %s", strerror(errno));
    pthread_mutex_lock(&io->lock);
    io->status = USBI_IO_COMPLETED_FAIL;
    pthread_mutex_unlock(&io->lock);
    return translate_errno(errno);
  }    
   
	/* Always do this to avoid race conditions */
	usbi_debug(hdev->lib_hdl,4,"URB Submitted. Waking Polling Thread");
	wakeup_io_thread(hdev);

  return 0;
}



/*
 * linux_submit_ctrl
 *
 *  Submits an io request to the control endpoint
 */
int32_t linux_submit_ctrl(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  libusb_ctrl_request_t *ctrl;
	uint8_t 							setup[USBI_CONTROL_SETUP_LEN];

  /* Validate... */
  if ((!hdev) || (!io)) {
    return (LIBUSB_BADARG);
  }

  /* allocate memory for the private part */
  io->priv = malloc(sizeof(struct usbi_io_private));
  if (!io->priv) {
    usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the private io member");
    return (LIBUSB_NO_RESOURCES);
  }
  memset(io->priv, 0, sizeof(*io->priv));
  
  /* get a pointer to the request */
  ctrl = io->req->req.ctrl;
	
	/* fill in the setup packet */
	setup[0] = ctrl->setup.bmRequestType;
	setup[1] = ctrl->setup.bRequest;
	*(uint16_t *)(setup + 2) = libusb_cpu_to_le16(ctrl->setup.wValue);
	*(uint16_t *)(setup + 4) = libusb_cpu_to_le16(ctrl->setup.wIndex);
	*(uint16_t *)(setup + 6) = libusb_cpu_to_le16(ctrl->length);
	
  /* setup the URB */
  io->priv->urb.type = USBK_URB_TYPE_CONTROL;
	
  /* allocate a temporary buffer for the payload */
  io->priv->urb.buffer = malloc(USBI_CONTROL_SETUP_LEN + ctrl->length);
  if (!io->priv->urb.buffer) {
    return (LIBUSB_NO_RESOURCES);
  }
	memset(io->priv->urb.buffer,0,USBI_CONTROL_SETUP_LEN + ctrl->length);
  
  /* fill in the temporary buffer */
  memcpy(io->priv->urb.buffer, setup, USBI_CONTROL_SETUP_LEN);
  io->priv->urb.buffer_length = USBI_CONTROL_SETUP_LEN + ctrl->length;
  
  /* copy the data if we're writing */
  if ((ctrl->setup.bmRequestType & USB_REQ_DIR_MASK) == USB_REQ_HOST_TO_DEV) {
    memcpy(io->priv->urb.buffer + USBI_CONTROL_SETUP_LEN, ctrl->payload, ctrl->length);
  }

  io->priv->urb.actual_length = 0;
  io->priv->urb.number_of_packets = 0;  

	return urb_submit(hdev, io);
}



/*
 * linux_submit_intr
 *
 *  Submits and io request to the interrupt endpoint
 */
int32_t linux_submit_intr(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  libusb_intr_request_t *intr;

  /* Validate... */
  if ((!hdev) || (!io)) {
    return (LIBUSB_BADARG);
  }
  
  /* allocate memory for the private part */
  io->priv = malloc(sizeof(struct usbi_io_private));
  if (!io->priv) {
    usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the private io member");
    return (LIBUSB_NO_RESOURCES);
  }
  memset(io->priv, 0, sizeof(*io->priv));
  
  /* get a pointer to the request */
  intr = io->req->req.intr; 

  /* create the urb */
  io->priv->urb.type = USBK_URB_TYPE_INTERRUPT;

  io->priv->urb.buffer = intr->payload;
  io->priv->urb.buffer_length = intr->length;

  io->priv->urb.actual_length = 0;
  io->priv->urb.number_of_packets = 0;

  return urb_submit(hdev, io);
}



/*
 * linux_submit_bulk
 *
 *  Submits an io request to the bulk endpoint
 */
int32_t linux_submit_bulk(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  libusb_bulk_request_t *bulk;

  /* Validate... */
  if ((!hdev) || (!io)) {
    return (LIBUSB_BADARG);
  }
  
  /* allocate memory for the private part */
  io->priv = malloc(sizeof(struct usbi_io_private));
  if (!io->priv) {
    usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the private io member");
    return (LIBUSB_NO_RESOURCES);
  }
  memset(io->priv, 0, sizeof(*io->priv));
  
  /* get a pointer to the request */
  bulk = io->req->req.bulk;
   
  /* create the URB */
  io->priv->urb.type = USBK_URB_TYPE_BULK;

  io->priv->urb.buffer = bulk->payload;
  io->priv->urb.buffer_length = bulk->length;

  io->priv->urb.actual_length = 0;
  io->priv->urb.number_of_packets = 0;

  return urb_submit(hdev, io);
}



/*
 * linux_submit_isoc
 *
 *  Submits an isochronous io request
 */
int32_t linux_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  int             n = 0;
  int             ret;
  struct usbi_io  *new_io;

  if((!io) || (!hdev)) {
    return LIBUSB_BADARG;
  }

  /* allocate memory for the private part */
  io->priv = malloc(sizeof(struct usbi_io_private));
  if (!io->priv) {
    usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the private io member");
    return (LIBUSB_NO_RESOURCES);
  }
  memset(io->priv, 0, sizeof(*io->priv)); 

  resubmit_flag = RESUBMIT;
  for(n = 0; n < ISOC_URB_MAX_NUM; n++) {
    usleep(1000);

    new_io = isoc_io_clone(io);
    if(new_io != NULL) {

      ret = urb_submit(hdev, new_io);
      if(ret < 0) {
        usbi_debug(hdev->lib_hdl, 1, "submit isoc urb error!\n", strerror(errno));
        return translate_errno(errno);
      }
    }
  }
  
  return LIBUSB_SUCCESS;
}



/*
 * isoc_io_clone
 *
 */
struct usbi_io* isoc_io_clone(struct usbi_io *io)
{
  struct usbi_io *new_io = NULL;
  struct usbk_iso_packet_desc *p;
  struct libusb_isoc_packet *packets;
  struct libusb_isoc_request *isocr;
  void *new_buf;
  char *tmp;
  int buflen = 0;
  int i = 0;
  int new_iolen;
  
  /*create new_io for descriptor space of num_packets*/
  new_io = (struct usbi_io *)malloc(sizeof(*io) + io->req->req.isoc->pkts.num_packets * sizeof(struct usbk_iso_packet_desc));
  
  if(!new_io) {
    usbi_debug(io->dev->lib_hdl, 1, "error malloc new_io: %s\n", strerror(errno));
    return NULL;
  } 
    
  new_iolen =   sizeof(*io) + io->req->req.isoc->pkts.num_packets
              * sizeof(struct usbk_iso_packet_desc);
  memset(new_io, 0, new_iolen);
  memcpy(new_io, io, sizeof(*io));

  new_io->priv = malloc(sizeof(*new_io->priv));
  
  if(!new_io) {
    usbi_debug(io->dev->lib_hdl, 1, "error malloc new_io: %s\n", strerror(errno));
    return NULL;
  }
  memset(new_io->priv, 0, sizeof(*new_io->priv)); 
  memcpy(new_io->priv, io->priv, sizeof(*io->priv)); 
  
  /*initialize new io structure*/
  pthread_mutex_init(&new_io->lock, NULL);
  pthread_cond_init(&new_io->cond, NULL);
  list_init(&new_io->list);
          
  new_io->priv->urb.type = USBK_URB_TYPE_ISO;
  isocr = new_io->req->req.isoc;
  packets = isocr->pkts.packets;
        
  /*isoc.num_packets seems no need, because isoc request has included this member*/
  new_io->req->req.isoc->pkts.num_packets = isocr->pkts.num_packets;
  for(i = 0; i < isocr->pkts.num_packets; i++) {
    buflen += packets[i].length;
  }
          
  new_buf = malloc(buflen);
  if(!new_buf) {
    free(new_io);
    return NULL;
  }
  memset(new_buf, 0, buflen);
  
  new_io->priv->urb.buffer = new_buf;
  new_io->priv->urb.buffer_length = buflen;
  new_io->priv->urb.start_frame = isocr->start_frame;
  new_io->priv->urb.number_of_packets = isocr->pkts.num_packets;
  new_io->priv->urb.flags = isocr->flags;
  
  p = &new_io->priv->urb.iso_frame_desc[0];
  tmp = new_io->priv->urb.buffer;
  for(i = 0; i < new_io->priv->urb.number_of_packets; i++) {
    p[i].length = packets[i].length;
    p[i].actual_length = 0;
    p[i].status = 0;

    /*while usb isoc out transfer*/
    if( (io->req->endpoint & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT) {
      memcpy((void *)tmp, packets[i].payload, packets[i].length);
      tmp += packets[i].length;
    }
  }

  return new_io;
}



/*
 * io_complete
 *
 *  This function is called by the poll_io thread when a submitted io request
 *  has been completed.
 */
int32_t io_complete(struct usbi_dev_handle *hdev)
{
  struct usbk_urb *urb;
	struct usbi_io  *io, *tio;
  struct usbi_io  *new_io;
  struct timeval  tvo = {.tv_sec = 0, .tv_usec = 0};
  struct libusb_isoc_packet *packets;
  int ret, i, offset = 0;

	usbi_debug(hdev->lib_hdl, 4, "Process Completed URB");
			
  while((ret = ioctl(hdev->priv->fd, IOCTL_USB_REAPURBNDELAY, (void *)&urb)) >= 0) {
    io = urb->usercontext;
    pthread_mutex_lock(&io->lock);

    /* if this was a control request copy the payload */
    if (io->req->type == USB_TYPE_CONTROL) {
      memcpy(io->req->req.ctrl->payload, urb->buffer + USBI_CONTROL_SETUP_LEN, io->req->req.ctrl->length);
      if(urb->buffer)
        free(urb->buffer);
    }

    /*complete handle for isochronous transfer*/
    if(io->req->type == USB_TYPE_ISOCHRONOUS) {
      /*copy data*/
      packets = io->req->req.isoc->pkts.packets;
      io->req->req.isoc->isoc_results = (libusb_request_result_t *)malloc(io->req->req.isoc->pkts.num_packets * sizeof(libusb_request_result_t));
      if(io->req->req.isoc->isoc_results == NULL) {
        if(urb->buffer)
          free(urb->buffer);
        free(io->priv);
        usbi_free_io(io);
        continue;
      }

      /*copy the result of each packet*/
      offset = 0;
      for(i = 0; i < io->req->req.isoc->pkts.num_packets; i++) {
        memcpy(packets[i].payload, urb->buffer + offset, packets[i].length);
        offset += packets[i].length;
        io->req->req.isoc->isoc_results[i].status = urb->iso_frame_desc[i].status;
        io->req->req.isoc->isoc_results[i].transferred_bytes = urb->iso_frame_desc[i].actual_length;
      }
      if(urb->buffer)
        free(urb->buffer);
      free(io->priv);
      
      /*resubmit isoc io*/
      if(resubmit_flag == RESUBMIT)
      {
        new_io = isoc_io_clone(io);
        if(new_io != NULL) {
          urb_submit(hdev, new_io);
        }
      }
    }

		io->status = USBI_IO_COMPLETED;
		pthread_mutex_unlock(&io->lock);

    /* urb->status == -2, indicates that the URB was discarded, aka canceled */
    if (urb->status == -2) {
			usbi_io_complete(io, USBI_IO_CANCEL, urb->actual_length);
		} else {
			usbi_io_complete(io, USBI_IO_COMPLETED, urb->actual_length);
		}
  }

	list_for_each_entry_safe(io, tio, &io->list, list) {
    if(io->req->type == USB_TYPE_ISOCHRONOUS) {
      continue;
    }
    if(!tvo.tv_sec || usbi_timeval_compare(&io->tvo, &tvo) < 0) {
      memcpy(&tvo, &io->tvo, sizeof(tvo));
    }
  }  

  if(!(tvo.tv_sec == 0 && tvo.tv_usec == 0)) {
    memcpy(&hdev->priv->tvo, &tvo, sizeof(hdev->priv->tvo));
  }

  return (LIBUSB_SUCCESS);
}



/*
 * io_timeout
 *
 *  This function is called by the poll_io thread when a submitted io request
 *  has timed out.
 */
int32_t io_timeout(struct usbi_dev_handle *hdev, struct timeval *tvc)
{
  struct timeval tvo = { .tv_sec = 0 };
  struct usbi_io *io, *tio;

  /* check each entry in the io list to find out if it's timed out */ 
  list_for_each_entry_safe(io, tio, &hdev->io_head, list) {
    
    pthread_mutex_lock(&io->lock);
      
    /*currently, isochronous io doesn't consider timeout issue*/
    if(io->req->type == USB_TYPE_ISOCHRONOUS) {
			pthread_mutex_unlock(&io->lock);
			continue;
    }

    if (usbi_timeval_compare(&hdev->priv->tvo, tvc) <= 0) {
      int ret;

      /* this request has timed out, tell usbfs to discard it */
      ret = ioctl(hdev->priv->fd, IOCTL_USB_DISCARDURB, &io->priv->urb);
      if (ret < 0) {
        usbi_debug(hdev->lib_hdl, 1, "error cancelling URB: %s", strerror(errno));
				pthread_mutex_unlock(&io->lock);
        return translate_errno(errno);
      }
      
      /* clear out the buffer if it exists */
      if(io->priv->urb.buffer)
        free(io->priv->urb.buffer);

      usbi_io_complete(io, LIBUSB_IO_TIMEOUT, 0);
    } else if (!tvo.tv_sec || usbi_timeval_compare(&io->tvo, &tvo) < 0) {
      /* New soonest timeout */
      memcpy(&tvo, &io->tvo, sizeof(tvo));
    }
    
    pthread_mutex_unlock(&io->lock);
  }

  memcpy(&hdev->priv->tvo, &tvo, sizeof(hdev->priv->tvo));

  return (LIBUSB_SUCCESS);
}



/*
 * linux_io_cancel
 *
 *  Called to cancel a pending io request. This function will fail if the io
 *  request has already been submitted to the usbfs kernel driver, even if it
 *  hasn't been completed.
 */
int32_t linux_io_cancel(struct usbi_io *io)
{
  int ret;

  /* Prevent anyone else from accessing the io */
  pthread_mutex_lock(&io->lock);

  /* Discard/Cancel the URB */
  ret = ioctl(io->dev->priv->fd, IOCTL_USB_DISCARDURB, &io->priv->urb);
  if (ret < 0) {
    usbi_debug(io->dev->lib_hdl, 1, "error cancelling URB: %s", strerror(errno));
    return translate_errno(errno);
  }

  /* Unlock... */
  pthread_mutex_unlock(&io->lock);

  /* Always do this to avoid race conditions */
  wakeup_io_thread(io->dev);

  return LIBUSB_SUCCESS;
}



/******************************************************************************
 *                             Thread Functions                               *
 *****************************************************************************/

/*
 * poll_io
 *
 *  This is our worker thread. It's a forever loop, so it must be shutdown with
 *  pthread_cancel (currently done in linux_fini).
 */
void *poll_io(void *devhdl)
{
	struct usbi_dev_handle  *hdev = (struct usbi_dev_handle*)devhdl;
	usbi_debug(hdev->lib_hdl, 4, "Begin IO Polling");
	/*
   * Loop forever checking to see if we have io requests that need to be
   * processed and process them.
   */ 
	while (1) {
    struct timeval          tvc, tvo;
    fd_set                  readfds, writefds;
    int                     ret, maxfd;  

    /* find out if there is any reading/writing to be done */
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    /* Always check the event_pipe and our device file descriptor */
    FD_SET(hdev->priv->event_pipe[0], &readfds);
    FD_SET(hdev->priv->fd, &writefds);
    
    /* get the max file descriptor for select() */
    if (hdev->priv->event_pipe[0] > hdev->priv->fd) {
      maxfd = hdev->priv->event_pipe[0];
    } else {
      maxfd = hdev->priv->fd;
    }

    /* get the time so that we can determine if any timeouts have passed */
    gettimeofday(&tvc, NULL);
    memset(&tvo, 0, sizeof(tvo));

    /* our next soonest timeout is given by hdev->priv->tvo */
    memcpy(&tvo, &hdev->priv->tvo, sizeof(hdev->priv->tvo));

    /* calculate the timeout for select() based on what we found above */
    if (!tvo.tv_sec) {
      /* Default to an hour from now */
      tvo.tv_sec = tvc.tv_sec + (60 * 60);
      tvo.tv_usec = tvc.tv_usec;
    } else if (usbi_timeval_compare(&tvo, &tvc) < 0)
      /* Don't give a negative timeout */
      memcpy(&tvo, &tvc, sizeof(tvo));

    /* Make tvo absolute time now */
    tvo.tv_sec -= tvc.tv_sec;
    if (tvo.tv_usec < tvc.tv_usec) {
      tvo.tv_sec--;
      tvo.tv_usec += (1000000 - tvc.tv_usec);
    } else
      tvo.tv_usec -= tvc.tv_usec;

    /* determine if we have file descriptors reading for reading/writing */
    printf("select(%d, { tv_sec = %d, tv_usec = %d })\n", maxfd + 1, (int)tvo.tv_sec, (int)tvo.tv_usec);
    ret = select(maxfd + 1, &readfds, &writefds, NULL, &tvo);
    printf("select() = %d\n", ret);
    if (ret < 0) {
      usbi_debug(hdev->lib_hdl, 1, "select() call failed: %s", strerror(errno));
      continue;
    }

    gettimeofday(&tvc, NULL);

    /* if there is data to be read on the event pipe read it and discard*/
    if (FD_ISSET(hdev->priv->event_pipe[0], &readfds)) {
      char buf[16];

      read(hdev->priv->event_pipe[0], buf, sizeof(buf));
    }

    /* now that we've waited for select, determine what action to take */
    /* Have any io requests completed? */
    if (FD_ISSET(hdev->priv->fd, &writefds)) {
      io_complete(hdev);
    }

    /* Has our next soonest timeout passed? */
    if (usbi_timeval_compare(&hdev->priv->tvo, &tvc) <= 0) {
      io_timeout(hdev, &tvc);
    }
  }

  return (NULL);
}



/*
 * poll_events
 *
 *  This is our second worker function. It's a forever loop and must be stopped
 *  with pthread_cancel (currently done in linux_fini). This function does a
 *  rescan of devices, using usbi_rescan_devices, every 500ms. Hopefully, that
 *  will be fast enough to reliably detect and connect/disconnect events without
 *  using up all of our CPU time.
 */
void *poll_events(void *unused)
{
  char            filename[PATH_MAX + 1];
  char            sysfsmnt[PATH_MAX + 1];
  int             fd = -1;
  int             usingSysfs = 0;
  struct timeval  tvc;
  struct timeval  tvLastRescan;

  /* initialize our time values -- these inits will prevent a rescan there
  * first time the function is called */
  gettimeofday(&tvc, NULL);
  gettimeofday(&tvLastRescan, NULL);
  
  /* Determine if we're using SYSFS or /proc/bus/usb/devices to check events */
  if (sysfs_get_mnt_path(sysfsmnt,PATH_MAX+1) == 0)
  {
    usingSysfs = 1;
  }
  else
  {
    snprintf(filename, sizeof(filename), "%s/devices", device_dir);
    fd = open(filename, O_RDONLY);
    if (fd < 0)
      usbi_debug(NULL, 0, "unable to open %s to check for topology changes", filename);
  }

  /* Loop forever, checking for events every 1 second */
  while(1) {
    /* We'll only do a rescan if it's been at least 1 sec since our
     * last rescan */
    gettimeofday(&tvc, NULL);
    if ( tvc.tv_sec - tvLastRescan.tv_sec >= 1) {
      /* do the rescan */
      if ( (fd >= 0) || usingSysfs ) {
        usbi_rescan_devices();
        gettimeofday(&tvLastRescan, NULL);
      }
    } else {
      usleep(1000000);
    }
  }
}



/*
 * device_is_new
 *
 *  This function attempts to determine if a device is new or if we've already
 *  seen it before. The original idea was to check the mtime to see if the
 *  device is new, however this won't work as the mtime will always be different.
 *  TODO: A new way is definitely needed!
 */
int32_t device_is_new(struct usbi_device *idev, uint16_t devnum)
{
  char filename[PATH_MAX + 1];
  struct stat st;

  /* Compare the mtime to ensure it's new */
  snprintf(idev->priv->filename, sizeof(filename) - 1, "%s/%03d", idev->bus->priv->filename, devnum);
  stat(filename, &st);

  if (st.st_mtime == idev->priv->mtime)
    /* mtime matches, not a new device */
    return LIBUSB_SUCCESS;

  /*
   * FIXME: mtime does not match. Maybe the USB drivers have been unloaded and
   * reloaded? We should probably track the mtime of the bus to catch this
   * case and detach all devices on the bus. We would then detect all of
   * the devices as new.
   */
  /*
  usbi_debug(1, "device %s previously existed, but mtime has changed",
	filename);
  */

  return 0;
}



/*
 * create_new_device
 *
 *  Allocate memory for the new device and fill in the require information.
 *  THIS IS THE FUNCTION RESPONSIBLE FOR READING AND PARSING THE DEVICE
 *  DESCRIPTORS!
 */
int32_t create_new_device(struct usbi_device **dev, struct usbi_bus *ibus,
                          uint16_t devnum, uint32_t max_children)
{
  struct usbi_device *idev;

  idev = malloc(sizeof(*idev));
  if (!idev) {
    return (LIBUSB_NO_RESOURCES);
  }  
  memset(idev, 0, sizeof(*idev));

  idev->priv = calloc(sizeof(struct usbi_dev_private), 1);
  if (!idev->priv) {
    free(idev);
    return (LIBUSB_NO_RESOURCES);
  }
  
  idev->devnum = devnum;
  snprintf(idev->priv->filename, sizeof(idev->priv->filename) - 1, "%s/%03d",
           ibus->priv->filename, idev->devnum);

  idev->nports = max_children;
  if (max_children) {
    idev->children = malloc(idev->nports * sizeof(idev->children[0]));
    if (!idev->children) {
      free(idev);
      return (LIBUSB_NO_RESOURCES);
    }

    memset(idev->children, 0, idev->nports * sizeof(idev->children[0]));
  }

  *dev = idev;
  ibus->priv->dev_by_num[devnum] = idev;
  return (LIBUSB_SUCCESS);

}



/*
 * check_usb_path
 *
 *  Verify that we have access to the specified path
 */
int32_t check_usb_path(const char *dirname)
{
  DIR *dir;
  struct dirent *entry;
  int found = 0;

  dir = opendir(dirname);
  if (!dir)
    return 0;

  while ((entry = readdir(dir)) != NULL) {
    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    /* We assume if we find any files that it must be the right place */
    found = 1;
    break;
  }

  closedir(dir);

  return found;
}



/*
 * check_usb_path
 *
 *  Write data to the event pipe to wakeup the io thread
 */
int32_t wakeup_io_thread(struct usbi_dev_handle *hdev)
{
  char buf[1] = { 0x01 };

  if (write(hdev->priv->event_pipe[1], buf, 1) < 1) {
    usbi_debug(hdev->lib_hdl, 1, "unable to write to event pipe: %s", strerror(errno));
    return translate_errno(errno);
  }
  
  return LIBUSB_SUCCESS;
}



/*
 * linux_get_raw_desc
 *
 *  Get the raw descriptor specified. These are already cached by
 *  create_new_device, so we'll just copy over the data we need
 */
int32_t linux_get_raw_desc(struct usbi_device *idev, uint8_t type,
                           uint8_t descidx, uint16_t langid,
                           uint8_t **buffer, uint16_t *buflen)
{
  uint8_t               *devdescr = NULL;
  uint8_t               *cfgdescr = NULL;
  struct usbi_raw_desc  *configs_raw = NULL;
  size_t                devdescrlen;
  size_t                count;
  usb_device_desc_t     device;
  int32_t               sts = LIBUSB_SUCCESS; 
  int32_t               i, fd, ret;
  
  /* Validate... */
  if ((!idev) || (!buflen)) {
    return (LIBUSB_BADARG);
  }

  /* Right now we're only setup to do device and config descriptors */
  if ((type != USB_DESC_TYPE_DEVICE) && (type != USB_DESC_TYPE_CONFIG)) {
    usbi_debug(NULL, 1, "unsupported descriptor type");
    return (LIBUSB_BADARG);
  } 

  /* Open the device */
  fd = device_open(idev);
  if (fd < 0) {
    usbi_debug(NULL, 1, "couldn't open %s: %s", idev->priv->filename, strerror(errno));
    return (LIBUSB_UNKNOWN_DEVICE);
  }

  /* The way USBFS works we always have to read the data in order, so start by
   * reading the device descriptor, no matter what descriptor we were asked for
   */
  devdescr = malloc(USBI_DEVICE_DESC_SIZE);
  if (!devdescr) {
    usbi_debug(NULL, 1, "unable to allocate memory for cached device descriptor");
		sts = LIBUSB_NO_RESOURCES;
		goto done;
  }

  /* read the device descriptor */ 
  ret = read(fd, devdescr, USBI_DEVICE_DESC_SIZE);
  if (ret < 0) {
    usbi_debug(NULL, 1, "couldn't read descriptor: %s", strerror(errno));
    sts = translate_errno(errno);
    goto done;
  }
  devdescrlen = USBI_DEVICE_DESC_SIZE;

  /* if we were asked for the device descriptor then, we're done */
  if (type == USB_DESC_TYPE_DEVICE) {  
    *buflen = (uint16_t)devdescrlen;
    *buffer = devdescr;
    goto done;
  }

  /* parse the device decriptor to get the number of configurations */ 
  libusb_parse_data("bbwbbbbwwwbbbb", devdescr, devdescrlen, &device, USBI_DEVICE_DESC_SIZE, &count);

  /* now we'll allocated memory for all of our config descriptors */
  configs_raw = malloc(device.bNumConfigurations * sizeof(configs_raw[0]));
  if (!configs_raw) {
    usbi_debug(NULL, 1, "unable to allocate memory for cached descriptors");
    sts = LIBUSB_NO_RESOURCES;
    goto done;
  }
  memset(configs_raw, 0, device.bNumConfigurations * sizeof(configs_raw[0]));

  for (i = 0; i < device.bNumConfigurations; i++) {

    uint8_t                 buf[8];
    struct usb_config_desc  cfg_desc;
    struct usbi_raw_desc    *cfgr = configs_raw + i;

    /* Get the first 8 bytes so we can figure out what the total length is */
    ret = read(fd, buf, 8);
    if (ret < 8) {
      if (ret < 0) {
        usbi_debug(NULL, 1, "unable to get descriptor: %s", strerror(errno));
      } else {
        usbi_debug(NULL, 1, "config descriptor too short (expected %d, got %d)", 8, ret);
      } 
      sts = translate_errno(errno);
      goto done;
    }

    libusb_parse_data("bbw", buf, 8, &cfg_desc, sizeof(cfg_desc), &count);
    cfgr->len = cfg_desc.wTotalLength;

    cfgr->data = calloc(cfgr->len,1);
    if (!cfgr->data) {
      usbi_debug(NULL, 1, "unable to allocate memory for descriptors");
      sts = translate_errno(errno);
      goto done;
    }

    /* Copy over the first 8 bytes we read */
    memcpy(cfgr->data, buf, 8);

    ret = read(fd, cfgr->data + 8, cfgr->len - 8);
    if (ret < cfgr->len - 8) {
      if (ret < 0) {    
        usbi_debug(NULL, 1, "unable to get descriptor: %s", strerror(errno));
      } else {
        usbi_debug(NULL, 1, "config descriptor too short (expected %d, got %d)", cfgr->len, ret);
      }

      cfgr->len = 0;
      free(cfgr->data);
      sts = translate_errno(errno);
      goto done;
    }

    /* if this is the descriptor we want then we'll return it and be done */
    if (i == descidx) {
      *buflen = cfgr->len;

      /* allocate memory for the buffer to return */
      cfgdescr = calloc(cfgr->len,1);
      if (!cfgdescr) {
        usbi_debug(NULL, 1, "unable to allocate memory for the descriptor");
        sts = LIBUSB_NO_RESOURCES;
        goto done;
      }

      /* copy the data we read */
      memcpy(cfgdescr, cfgr->data, cfgr->len);
      *buffer = cfgdescr;
    }

    /* free the temporary memory */
    free(cfgr->data);
  }

done:

  /* Don't free the decdescr if that's what we got */
  close(fd);
  if (type != USB_DESC_TYPE_DEVICE)
  {
    if (devdescr) { free(devdescr); }
  }
  if (configs_raw) { free (configs_raw); }
 
  return (sts);
}



/*
 *	linux_get_driver
 *
 *		Gets the name of the kernel driver currently attached to the interface
 */
int32_t linux_get_driver(struct usbi_dev_handle *hdev, uint8_t interface,
												 char *name, size_t namelen)
{
  struct usbk_getdriver getdrv;
  int ret;

  getdrv.interface = interface;
  ret = ioctl(hdev->priv->fd, IOCTL_USB_GETDRIVER, &getdrv);
  if (ret) {
    usbi_debug(hdev->lib_hdl, 1,
							 "could not get bound driver: %s",
							 strerror(errno));
    return translate_errno(errno);
  }

  strncpy(name, getdrv.driver, namelen - 1);
  name[namelen - 1] = 0;

  return LIBUSB_SUCCESS;
}



/*
 *	linux_attach_kernel_driver
 *
 *		Attachs the appropriate kernel driver to the interface
 */
int32_t linux_attach_kernel_driver(struct usbi_dev_handle *hdev,
																	 uint8_t interface)
{
  struct usbk_ioctl command;
  int ret;

  command.ifno = interface;
  command.ioctl_code = IOCTL_USB_CONNECT;
  command.data = NULL;

  ret = ioctl(hdev->priv->fd, IOCTL_USB_IOCTL, &command);
  if (ret) {
		usbi_debug(hdev->lib_hdl, 1,
							 "could not attach kernel driver to interface %d: %s",
							 strerror(errno));
    return translate_errno(errno);
  }

	return (LIBUSB_SUCCESS);
}



/*
 *	linux_detach_kernal_driver
 *
 *		Detachs the kernel driver currently attached to the specified interface
 */
int32_t linux_detach_kernel_driver(struct usbi_dev_handle *hdev,
																	 uint8_t interface)
{
  struct usbk_ioctl command;
  int ret;

	command.ifno = interface;
	command.ioctl_code = IOCTL_USB_DISCONNECT;
	command.data = NULL;

	ret = ioctl(hdev->priv->fd, IOCTL_USB_IOCTL, &command);
	if (ret) {
		usbi_debug(hdev->lib_hdl, 1,
							 "could not detach kernel driver to interface %d: %s",
							 strerror(errno));
		return translate_errno(errno);
	}

	return (LIBUSB_SUCCESS);
}



struct usbi_backend_ops backend_ops = {
  .backend_version      = 1,
  .io_pattern           = PATTERN_ASYNC,
  .init                 = linux_init,
  .fini                 = linux_fini,
  .find_buses           = linux_find_buses,
  .refresh_devices      = linux_refresh_devices,
  .free_device          = linux_free_device,
  .dev = {
    .open               = linux_open,
    .close              = linux_close,
    .set_configuration  = linux_set_configuration,
    .get_configuration  = linux_get_configuration,
    .claim_interface    = linux_claim_interface,
    .release_interface  = linux_release_interface,
    .get_altsetting     = linux_get_altsetting,
    .set_altsetting     = linux_set_altsetting,
    .reset              = linux_reset,
    .ctrl_xfer_aio      = linux_submit_ctrl,
    .intr_xfer_aio      = linux_submit_intr,
    .bulk_xfer_aio      = linux_submit_bulk,
    .isoc_xfer_aio      = linux_submit_isoc,
    .ctrl_xfer_wait     = NULL,
    .intr_xfer_wait     = NULL,
    .bulk_xfer_wait     = NULL,
    .isoc_xfer_wait     = NULL,
    .io_cancel          = linux_io_cancel,
    .get_raw_desc       = linux_get_raw_desc,
  },
};
