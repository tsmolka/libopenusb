/*
 * Linux USB support
 *
 * Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
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

static pthread_t event_thread;
static int event_pipe[2];

static pthread_mutex_t usbi_ios_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head usbi_ios = { .prev = &usbi_ios, .next = &usbi_ios };

static char device_dir[PATH_MAX + 1] = "";


static int translate_errno(int errnum)
{
  switch(errnum)
  {
    default:
      return (LIBUSB_FAILURE);

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


static int device_open(struct usbi_device *idev)
{
  int fd;

  fd = open(idev->filename, O_RDWR);
  if (fd < 0) {
    fd = open(idev->filename, O_RDONLY);
    if (fd < 0) {
      usbi_debug(1, "failed to open %s: %s", idev->filename, strerror(errno));
      return translate_errno(errno);
    }
  }
  
  return fd;
}

static int linux_open(struct usbi_dev_handle *hdev)
{
  struct usbi_device *idev = hdev->idev;

  hdev->fd = device_open(idev);
  if (hdev->fd == LIBUSB_FAILURE)
    return LIBUSB_FAILURE;

  list_init(&hdev->ios);

  /* there's no need to get the current configuration here and set it, that's
   * already handled by the kernel driver.
   */

  return LIBUSB_SUCCESS;
}

static int linux_close(struct usbi_dev_handle *hdev)
{
  if (hdev->fd < 0)
    return 0;

  if (close(hdev->fd) == -1)
    /* Failing trying to close a file really isn't an error, so return 0 */
    usbi_debug(2, "error closing device fd %d: %s", hdev->fd, strerror(errno));

  return 0;
}

static int linux_set_configuration(struct usbi_dev_handle *hdev, int cfg)
{
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_SETCONFIG, &cfg);
  if (ret < 0) {
    usbi_debug(1, "could not set config %u: %s", cfg, strerror(errno));
    return translate_errno(errno);
  }

  hdev->idev->cur_config = cfg;

  return LIBUSB_SUCCESS;
}

static int linux_get_configuration(struct usbi_dev_handle *hdev, int *cfg)
{
  /* There's no IOCTL for querying the current confiruation, so let's just this
   * info from what we have cached.
   */ 
  *cfg = hdev->idev->cur_config;

  return 0;
}

static int linux_claim_interface(struct usbi_dev_handle *hdev, int interface)
{
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_CLAIMINTF, &interface);
  if (ret < 0) {
    usbi_debug(1, "could not claim interface %d: %s", interface, strerror(errno));
    return LIBUSB_FAILURE;
  }

  hdev->interface = interface;

  /* There's no usbfs IOCTL for querying the current alternate setting, we'll
   * leave it up to the user to set later.
   */

  return 0;
}

static int linux_release_interface(struct usbi_dev_handle *hdev, int interface)
{
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_RELEASEINTF, &interface);
  if (ret < 0) {
    usbi_debug(1, "could not release interface %d: %s", interface, strerror(errno));
    return translate_errno(errno);
  }

  hdev->interface = -1;

  return 0;
}

static int linux_set_altinterface(struct usbi_dev_handle *hdev, int alt)
{
  struct usbk_setinterface setintf;
  int ret;

  if (hdev->interface < 0)
    return LIBUSB_BADARG;

  setintf.interface = hdev->interface;
  setintf.altsetting = alt;

  ret = ioctl(hdev->fd, IOCTL_USB_SETINTF, &setintf);
  if (ret < 0) {
    /* FIXME: Add device number of filename to debug message? */
    usbi_debug(1, "could not set alternate interface %d/%d, errno = %d", hdev->interface, alt, errno);

    return translate_errno(errno);
  }

  hdev->altsetting = alt;

  return 0;
}

static int linux_get_altinterface(struct usbi_device *idev, int *alt)
{
  /* FIXME: Query the kernel for this information */
  return LIBUSB_FAILURE;
}

static int wakeup_event_thread(void)
{
  char buf[1] = { 0x01 };

  if (write(event_pipe[1], buf, 1) < 1) {
    usbi_debug(1, "unable to write to event pipe: %s", strerror(errno));
    return translate_errno(errno);
  }
  
  return LIBUSB_SUCCESS;
}

static int urb_submit(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  int ret;

  io->urb.endpoint = io->endpoint;
  io->urb.status = 0;
  io->urb.flags = 0;

  io->urb.signr = 0;
  io->urb.usercontext = (void *)io;

  io->inprogress = 1;

  pthread_mutex_lock(&usbi_ios_lock);
  if (list_empty(&hdev->ios)) {
    list_add(&hdev->io_list, &usbi_ios);
    memcpy(&hdev->tvo, &io->tvo, sizeof(hdev->tvo));
  } else if (usbi_timeval_compare(&io->tvo, &hdev->tvo) < 0)
    memcpy(&hdev->tvo, &io->tvo, sizeof(hdev->tvo));

  list_add(&io->list, &hdev->ios);
  pthread_mutex_unlock(&usbi_ios_lock);

  ret = ioctl(hdev->fd, IOCTL_USB_SUBMITURB, &io->urb);
  if (ret < 0) {
    usbi_debug(1, "error submitting URB: %s", strerror(errno));

    pthread_mutex_lock(&usbi_ios_lock);
    io->inprogress = 0;
    list_del(&hdev->io_list);
    list_del(&io->list);
    pthread_mutex_unlock(&usbi_ios_lock);

    return translate_errno(errno);
  }

  /* Always do this to avoid race conditions */
  wakeup_event_thread();

  return 0;
}

static int linux_submit_ctrl(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  io->urb.type = USBK_URB_TYPE_CONTROL;

  io->tempbuf = malloc(USBI_CONTROL_SETUP_LEN + io->ctrl.buflen);
  if (!io->tempbuf)
    return LIBUSB_NO_RESOURCES;

  memcpy(io->tempbuf, io->ctrl.setup, USBI_CONTROL_SETUP_LEN);
  /* FIXME: Only do this on writes? */
  memcpy(io->tempbuf + USBI_CONTROL_SETUP_LEN, io->ctrl.buf, io->ctrl.buflen);

  io->urb.buffer = io->tempbuf;
  io->urb.buffer_length = USBI_CONTROL_SETUP_LEN + io->ctrl.buflen;

  io->urb.actual_length = 0;
  io->urb.number_of_packets = 0;

  return urb_submit(hdev, io);
}

static int linux_submit_intr(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  io->urb.type = USBK_URB_TYPE_INTERRUPT;

  io->urb.buffer = io->intr.buf;
  io->urb.buffer_length = io->intr.buflen;

  io->urb.actual_length = 0;
  io->urb.number_of_packets = 0;

  return urb_submit(hdev, io);
}

static int linux_submit_bulk(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  io->urb.type = USBK_URB_TYPE_BULK;

  io->urb.buffer = io->bulk.buf;
  io->urb.buffer_length = io->bulk.buflen;

  io->urb.actual_length = 0;
  io->urb.number_of_packets = 0;

  return urb_submit(hdev, io);
}

static int linux_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
  /* FIXME: Implement */
  return LIBUSB_NOT_SUPPORTED;
}

/* FIXME: Make sure there aren't any race conditions here */
static int io_complete(struct usbi_dev_handle *hdev)
{
  struct usbk_urb *urb;
  struct usbi_io *io;
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_REAPURBNDELAY, (void *)&urb);
  if (ret < 0) {
    usbi_debug(1, "error reaping URB: %s", strerror(errno));
    return translate_errno(errno);
  }

  io = urb->usercontext;
  list_del(&io->list);		/* lock obtained by caller */

  if (io->type == USBI_IO_CONTROL && io->ctrl.setup)
    memcpy(io->ctrl.buf, io->urb.buffer + USBI_CONTROL_SETUP_LEN, io->ctrl.buflen);

  /* urb->status == -2, indicates that the URB was discarded, aka canceled */
  if (urb->status == -2)
    urb->status = LIBUSB_IO_CANCELED;
  
  usbi_io_complete(io, urb->status, urb->actual_length);

  return LIBUSB_SUCCESS;
}

static int io_timeout(struct usbi_dev_handle *hdev, struct timeval *tvc)
{
  struct timeval tvo = { .tv_sec = 0 };
  struct usbi_io *io, *tio;

  list_for_each_entry_safe(io, tio, &hdev->ios, list) {
    if (usbi_timeval_compare(&io->tvo, tvc) <= 0) {
      int ret;

      list_del(&io->list);

      ret = ioctl(hdev->fd, IOCTL_USB_DISCARDURB, &io->urb);
      if (ret < 0) {
        usbi_debug(1, "error cancelling URB: %s", strerror(errno));
        return translate_errno(errno);
      }

      usbi_io_complete(io, LIBUSB_IO_TIMEOUT, 0);
    } else if (!tvo.tv_sec || usbi_timeval_compare(&io->tvo, &tvo) < 0)
      /* New soonest timeout */
      memcpy(&tvo, &io->tvo, sizeof(tvo));
  }

  memcpy(&hdev->tvo, &tvo, sizeof(hdev->tvo));

  return LIBUSB_SUCCESS;
}

static int linux_io_cancel(struct usbi_io *io)
{
  int ret;

  /* Prevent anyone else from accessing the io */
  pthread_mutex_lock(&io->lock);
  
  /* Discard/Cancel the URB */
  ret = ioctl(io->dev->fd, IOCTL_USB_DISCARDURB, &io->urb);
  if (ret < 0) {
    usbi_debug(1, "error cancelling URB: %s", strerror(errno));
    return translate_errno(errno);
  }

  /* Unlock... */
  pthread_mutex_unlock(&io->lock);

  /* Always do this to avoid race conditions */
  wakeup_event_thread();

  return LIBUSB_SUCCESS;
}

static void *poll_events(void *unused)
{
  char filename[PATH_MAX + 1];
  char sysfsmnt[PATH_MAX + 1]; 
  int  fd = -1;
  int  usingSysfs = 0; 
  
  if (sysfs_get_mnt_path(sysfsmnt,PATH_MAX+1) == 0)
  {
    usingSysfs = 1;
  }
  else
  {
    snprintf(filename, sizeof(filename), "%s/devices", device_dir);
    fd = open(filename, O_RDONLY);
    if (fd < 0)
      usbi_debug(0, "unable to open %s to check for topology changes", filename);
  }
  
  while (1) {
    struct usbi_dev_handle *dev, *tdev;
    struct timeval tvc, tvo;  
    struct timeval tvLastRescan;  
    fd_set readfds, writefds;
    int ret, maxfd, doRescan = 1;

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    /* Always check the event_pipe and the devices file */
    FD_SET(event_pipe[0], &readfds);
    if (fd >= 0) {
      FD_SET(fd, &readfds);
		}

	  maxfd = fd > event_pipe[0] ? fd : event_pipe[0];
    
    gettimeofday(&tvc, NULL);

    memset(&tvo, 0, sizeof(tvo));

    /* determine if we have any file descriptors to check for writes and find
     * the next soonest timeout so select() knows how long to wait */  
    pthread_mutex_lock(&usbi_ios_lock);
    list_for_each_entry(dev, &usbi_ios, io_list) {
      FD_SET(dev->fd, &writefds);

      if (dev->fd > maxfd)
        maxfd = dev->fd;

      if (dev->tvo.tv_sec &&
          (!tvo.tv_sec || usbi_timeval_compare(&dev->tvo, &tvo)))
        /* New soonest timeout */
        memcpy(&tvo, &dev->tvo, sizeof(tvo));
    }
    pthread_mutex_unlock(&usbi_ios_lock);

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
    /*printf("select(%d, { tv_sec = %d, tv_usec = %d })\n", maxfd + 1, (int)tvo.tv_sec, (int)tvo.tv_usec);*/
    ret = select(maxfd + 1, &readfds, &writefds, NULL, &tvo);
    /*printf("select() = %d\n", ret);*/
    if (ret < 0) {
      usbi_debug(1, "select() call failed: %s", strerror(errno));
      continue;
    }

    gettimeofday(&tvc, NULL);

    if (FD_ISSET(event_pipe[0], &readfds)) {
      char buf[16];

      read(event_pipe[0], buf, sizeof(buf));
    }

    /* We'll only do a rescan if it's been at least 500ms since our
     * last rescan */
    if ( tvc.tv_sec - tvLastRescan.tv_sec > 1) {
      doRescan = 1;
    } else if (tvc.tv_usec - tvLastRescan.tv_usec > 500000) {
      doRescan = 1;
    } else {
      doRescan = 0;
    }
    if (doRescan) {
      if ( (fd >= 0 && FD_ISSET(fd, &readfds)) || usingSysfs) {
        /* FIXME: We need to handle new/removed busses as well */
        usbi_rescan_devices();
        gettimeofday(&tvLastRescan, NULL);
      }
    }

    /* now we'll process any pending io requests & timeouts */
    pthread_mutex_lock(&usbi_ios_lock);
    list_for_each_entry_safe(dev, tdev, &usbi_ios, io_list) {
      if (FD_ISSET(dev->fd, &writefds))
        io_complete(dev);

      if (usbi_timeval_compare(&dev->tvo, &tvc) <= 0)
        io_timeout(dev, &tvc);

      if (list_empty(&dev->ios))
        list_del(&dev->io_list);
    }
    pthread_mutex_unlock(&usbi_ios_lock);
  
  }

  return NULL;
}

static int linux_find_busses(struct list_head *busses)
{
  DIR *dir;
  struct dirent *entry;

  /* Scan /proc/bus/usb for bus named directories */

  dir = opendir(device_dir);
  if (!dir) {
    usbi_debug(1, "could not opendir(%s): %s", device_dir, strerror(errno));
    return translate_errno(errno);
  }

  while ((entry = readdir(dir)) != NULL) {
    struct usbi_bus *ibus;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    if (!strchr("0123456789", entry->d_name[strlen(entry->d_name) - 1])) {
      usbi_debug(2, "skipping non bus dir %s", entry->d_name);
      continue;
    }

    /* FIXME: Centralize allocation and initialization */
    ibus = malloc(sizeof(*ibus));
    if (!ibus)
      return LIBUSB_NO_RESOURCES;

    memset(ibus, 0, sizeof(*ibus));

    pthread_mutex_init(&ibus->lock, NULL);

    ibus->busnum = atoi(entry->d_name);
    snprintf(ibus->filename, sizeof(ibus->filename), "%s/%s",
	device_dir, entry->d_name);

    list_add(&ibus->list, busses);

    usbi_debug(2, "found bus dir %s", ibus->filename);
  }

  closedir(dir);

  return LIBUSB_SUCCESS;
}

static int device_is_new(struct usbi_device *idev, unsigned short devnum)
{
  char filename[PATH_MAX + 1];
  struct stat st;

  /* Compare the mtime to ensure it's new */
  snprintf(filename, sizeof(filename) - 1, "%s/%03d", idev->bus->filename, devnum);
  stat(filename, &st);

  if (st.st_mtime == idev->mtime)
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

static int create_new_device(struct usbi_device **dev, struct usbi_bus *ibus,
	unsigned short devnum, unsigned int max_children)
{
  struct usbi_device *idev;
  int i, ret;
  int fd;
  size_t count;

  idev = malloc(sizeof(*idev));
  if (!idev)
    return LIBUSB_NO_RESOURCES;

  memset(idev, 0, sizeof(*idev));

  idev->devnum = devnum;
  snprintf(idev->filename, sizeof(idev->filename) - 1, "%s/%03d",
	ibus->filename, idev->devnum);

  idev->num_ports = max_children;
  if (max_children) {
    idev->children = malloc(idev->num_ports * sizeof(idev->children[0]));
    if (!idev->children) {
      free(idev);
      return LIBUSB_NO_RESOURCES;
    }

    memset(idev->children, 0, idev->num_ports * sizeof(idev->children[0]));
  }

  fd = device_open(idev);
  if (fd < 0) {
    usbi_debug(2, "couldn't open %s: %s", idev->filename, strerror(errno));

    free(idev);
    return LIBUSB_UNKNOWN_DEVICE;
  }

  /* FIXME: Better error messages (with filename for instance) */

  /* FIXME: Fetch the size of the descriptor first, then the rest. This is needed for forward compatibility */

  idev->desc.device_raw.data = malloc(USBI_DEVICE_DESC_SIZE);
  if (!idev->desc.device_raw.data) {
    usbi_debug(1, "unable to allocate memory for cached device descriptor");
    goto done;
  }

  ret = read(fd, idev->desc.device_raw.data, USBI_DEVICE_DESC_SIZE);
  if (ret < 0) {
    usbi_debug(1, "couldn't read descriptor: %s", strerror(errno));
    goto done;
  }

  idev->desc.device_raw.len = USBI_DEVICE_DESC_SIZE;
  libusb_parse_data("..wbbbbwwwbbbb", idev->desc.device_raw.data, idev->desc.device_raw.len, &idev->desc.device, USBI_DEVICE_DESC_SIZE, &count);

  usbi_debug(2, "found device %03d on %s", idev->devnum, ibus->filename);

  /* Now try to fetch the rest of the descriptors */
  if (idev->desc.device.bNumConfigurations > USBI_MAXCONFIG)
    /* Silent since we'll try again later */
    goto done;

  if (idev->desc.device.bNumConfigurations < 1)
    /* Silent since we'll try again later */
    goto done;

  idev->desc.configs_raw = malloc(idev->desc.device.bNumConfigurations * sizeof(idev->desc.configs_raw[0]));
  if (!idev->desc.configs_raw) {
    usbi_debug(1, "unable to allocate memory for cached descriptors");

    goto done;
  }

  memset(idev->desc.configs_raw, 0, idev->desc.device.bNumConfigurations * sizeof(idev->desc.configs_raw[0]));

  idev->desc.configs = malloc(idev->desc.device.bNumConfigurations * sizeof(idev->desc.configs[0]));
  if (!idev->desc.configs)
    /* Silent since we'll try again later */
    goto done;

  idev->desc.num_configs = idev->desc.device.bNumConfigurations;

  memset(idev->desc.configs, 0, idev->desc.num_configs * sizeof(idev->desc.configs[0]));

  for (i = 0; i < idev->desc.num_configs; i++) {
    unsigned char buf[8];
    struct usb_config_desc cfg_desc;
    struct usbi_raw_desc *cfgr = idev->desc.configs_raw + i;
/*    size_t count; */

    /* Get the first 8 bytes so we can figure out what the total length is */
    ret = read(fd, buf, 8);
    if (ret < 8) {
      if (ret < 0)
        usbi_debug(1, "unable to get descriptor: %s", strerror(errno));
      else
        usbi_debug(1, "config descriptor too short (expected %d, got %d)", 8, ret);

      goto done;
    }

    libusb_parse_data("..w", buf, 8, &cfg_desc, sizeof(cfg_desc), &count);
    cfgr->len = cfg_desc.wTotalLength;

    cfgr->data = malloc(cfgr->len);
    if (!cfgr->data) {
      usbi_debug(1, "unable to allocate memory for descriptors");
      ret = LIBUSB_NO_RESOURCES;
      goto err;
    }

    /* Copy over the first 8 bytes we read */
    memcpy(cfgr->data, buf, 8);

    ret = read(fd, cfgr->data + 8, cfgr->len - 8);
    if (ret < cfgr->len - 8) {
      if (ret < 0)
        usbi_debug(1, "unable to get descriptor: %s", strerror(errno));
      else
        usbi_debug(1, "config descriptor too short (expected %d, got %d)", cfgr->len, ret);

      cfgr->len = 0;
      free(cfgr->data);

      goto done;
    }

    ret = usbi_parse_configuration(idev->desc.configs + i, cfgr->data, cfgr->len);
    if (ret > 0)
      usbi_debug(2, "%d bytes of descriptor data still left", ret);
    else if (ret < 0)
      usbi_debug(1, "unable to parse descriptors");
  }

done:
  *dev = idev;

  ibus->dev_by_num[devnum] = idev;

  close(fd);

  return 0;

err:
  close(fd);
  free(idev);

  return translate_errno(errno);
}

static int linux_refresh_devices(struct usbi_bus *ibus)
{
	char devfilename[PATH_MAX + 1];
	struct usbi_device *idev, *tidev;
	int busnum = 0, pdevnum = 0, pport = 0, devnum = 0, max_children = 0;
	char sysfsdir[PATH_MAX + 1];
	FILE *f;
  
  /* we'll use libsysfs as our primary way of getting the device topology */  
  if (sysfs_get_mnt_path(sysfsdir,PATH_MAX+1) == 0)
  {
    struct sysfs_class *class;
    struct sysfs_class_device *classdev;
    struct sysfs_device *dev;
    struct sysfs_device *parentdev;
    struct sysfs_attribute *devattr;
    struct dlist *devlist;
    char *p, *b, *d, *t;
  
    pthread_mutex_lock(&ibus->lock);

    /* Reset the found flag for all devices */
    list_for_each_entry(idev, &ibus->devices, bus_list)
      idev->found = 0;
  
    /* if we were able to get the mount path, it's safe to say we support
     * sysfs and we'll use that to get our info
     */
    class = sysfs_open_class("usb_device");
    if (!class) {
      usbi_debug(1, "could not open sysfs usb_device class: %s\n",strerror(errno));
      return translate_errno(errno);
    }
  
    devlist = sysfs_get_class_devices(class);
    if (!devlist) {
      usbi_debug(1, "could not get sysfs/bus/usb devices: %s",strerror(errno));
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
          usbi_debug(1, "could not open sysfs device this class device (%s): %s\n",classdev->name,strerror(errno));
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
          usbi_debug(1, "invalid device number, max children or parent device");
          continue;
        }

        /* Validate the parent device */
        if (pdevnum && (!ibus->dev_by_num[pdevnum])) {
          usbi_debug(1, "no parent device or invalid child port number");
          continue;
        }

        /* make sure we don't have two root devices */
        if (!pdevnum && ibus->root && ibus->root->found) {
           usbi_debug(1, "cannot have two root devices");
           continue;
        }

        /* Only add this device if it's new */

        /* If we don't have a device by this number yet, it must be new */
        idev = ibus->dev_by_num[devnum];
        if (idev && device_is_new(idev, devnum))
          idev = NULL;

        if (!idev) {
           int ret;

           ret = create_new_device(&idev, ibus, devnum, max_children);
           if (ret) {
             usbi_debug(1, "ignoring new device because of errors");
             continue;
           }
  
           usbi_add_device(ibus, idev);

           /* Setup parent/child relationship */
           if (pdevnum) {
             ibus->dev_by_num[pdevnum]->children[pport] = idev;
             idev->parent = ibus->dev_by_num[pdevnum];
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
      usbi_debug(1, "could not open %s: %s", devfilename, strerror(errno));
      return translate_errno(errno);
    }   
      
		pthread_mutex_lock(&ibus->lock);

		/* Reset the found flag for all devices */
		list_for_each_entry(idev, &ibus->devices, bus_list)
				idev->found = 0;

		while (!feof(f)) {
//    int busnum = 0, pdevnum = 0, pport = 0, devnum = 0, max_children = 0;
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
						usbi_debug(1, "invalid device number, max children or parent device");
						break;
					}

					/* Validate the parent device */
					if (pdevnum && (!ibus->dev_by_num[pdevnum] ||
							pport >= ibus->dev_by_num[pdevnum]->num_ports)) {
						usbi_debug(1, "no parent device or invalid child port number");
						break;
					}

					if (!pdevnum && ibus->root && ibus->root->found) {
						usbi_debug(1, "cannot have two root devices");
						break;
					}

					/* Only add this device if it's new */

					/* If we don't have a device by this number yet, it must be new */
					idev = ibus->dev_by_num[devnum];
					if (idev && device_is_new(idev, devnum))
						idev = NULL;

					if (!idev) {
						int ret;

						ret = create_new_device(&idev, ibus, devnum, max_children);
						if (ret) {
							usbi_debug(1, "ignoring new device because of errors");
							break;
						}

						usbi_add_device(ibus, idev);

						/* Setup parent/child relationship */
						if (pdevnum) {
							ibus->dev_by_num[pdevnum]->children[pport] = idev;
							idev->parent = ibus->dev_by_num[pdevnum];
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
	
	list_for_each_entry_safe(idev, tidev, &ibus->devices, bus_list) {
		if (!idev->found) {
			/* Device disappeared, remove it */
			usbi_debug(2, "device %d removed", idev->devnum);
			usbi_remove_device(idev);
		}
	}

	pthread_mutex_unlock(&ibus->lock);

	return LIBUSB_SUCCESS;
}

static int check_usb_path(const char *dirname)
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

static int linux_init(void)
{
  int  ret;
  char sysfsdir[PATH_MAX+1];
  
  /* Find the path to the directory tree with the device nodes */
  if (getenv("USB_PATH")) {
    if (check_usb_path(getenv("USB_PATH"))) {
      strncpy(device_dir, getenv("USB_PATH"), sizeof(device_dir) - 1);
      device_dir[sizeof(device_dir) - 1] = 0;
    } else
      usbi_debug(1, "couldn't find USB devices in USB_PATH");
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
      device_dir[0] = 0;	/* No path, no USB support */
  }

  if (device_dir[0])
    usbi_debug(1, "found USB device directory at %s", device_dir);
  else
    usbi_debug(1, "no USB device directory found");

  pipe(event_pipe);

  /* Start up thread for polling events */
  ret = pthread_create(&event_thread, NULL, poll_events, NULL);
  if (ret < 0)
    usbi_debug(1, "unable to create polling thread (ret = %d)", ret);

  /* FIXME: Wait until first scan of devices is finished */

  return LIBUSB_SUCCESS;
}

int libusb_clear_halt(struct usbi_dev_handle *hdev, unsigned char ep)
{
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_CLEAR_HALT, &ep);
  if (ret) {
    usbi_debug(1, "could not clear halt ep %d: %s", ep, strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}

static int linux_reset(struct usbi_dev_handle *hdev)
{
  int ret;

  ret = ioctl(hdev->fd, IOCTL_USB_RESET, NULL);
  if (ret) {
    usbi_debug(1, "could not reset: %s", strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}

static int linux_get_driver(struct usbi_dev_handle *hdev, int interface,
	char *name, size_t namelen)
{
  struct usbk_getdriver getdrv;
  int ret;

  getdrv.interface = interface;
  ret = ioctl(hdev->fd, IOCTL_USB_GETDRIVER, &getdrv);
  if (ret) {
    usbi_debug(1, "could not get bound driver: %s", strerror(errno));
    return translate_errno(errno);
  }

  strncpy(name, getdrv.driver, namelen - 1);
  name[namelen - 1] = 0;

  return LIBUSB_SUCCESS;
}

static int linux_attach_kernel_driver(struct usbi_dev_handle *hdev,
	int interface)
{
  struct usbk_ioctl command;
  int ret;

  command.ifno = interface;
  command.ioctl_code = IOCTL_USB_CONNECT;
  command.data = NULL;

  ret = ioctl(hdev->fd, IOCTL_USB_IOCTL, &command);
  if (ret) {
    usbi_debug(1, "could not attach kernel driver to interface %d: %s", strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}

static int linux_detach_kernel_driver(struct usbi_dev_handle *hdev,
	int interface)
{
  struct usbk_ioctl command;
  int ret;

  command.ifno = interface;
  command.ioctl_code = IOCTL_USB_DISCONNECT;
  command.data = NULL;

  ret = ioctl(hdev->fd, IOCTL_USB_IOCTL, &command);
  if (ret) {
    usbi_debug(1, "could not detach kernel driver to interface %d: %s", strerror(errno));
    return translate_errno(errno);
  }

  return LIBUSB_SUCCESS;
}


int backend_version = 1;
int backend_io_pattern = PATTERN_ASYNC;

struct usbi_backend_ops backend_ops = {
  .init       = linux_init,
  .find_busses      = linux_find_busses,
  .refresh_devices    = linux_refresh_devices,
  .free_device      = NULL,
  .dev = {
    .open     = linux_open,
    .close      = linux_close,
    .set_configuration    = linux_set_configuration,
    .get_configuration    = linux_get_configuration,
    .claim_interface    = linux_claim_interface,
    .release_interface    = linux_release_interface,
    .get_altinterface   = linux_get_altinterface,
    .set_altinterface   = linux_set_altinterface,
    .reset      = linux_reset,
    .get_driver_np    = linux_get_driver,
    .attach_kernel_driver_np  = linux_attach_kernel_driver,
    .detach_kernel_driver_np  = linux_detach_kernel_driver,
    .submit_ctrl    = linux_submit_ctrl,
    .submit_intr    = linux_submit_intr,
    .submit_bulk    = linux_submit_bulk,
    .submit_isoc    = linux_submit_isoc,
    .io_cancel      = linux_io_cancel,
  },
};
