/*
 * Linux USB support
 *
 *	Copyright 2007-2008 Michael Lewis <milewis1@gmail.com>
 *	Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 *	This library is covered by the LGPL, read LICENSE for details.
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
#include <math.h>

#include "usbi.h"
#include "linux.h"

#define	LINUX_MAX_BULK_INTR_XFER	16384
#define LINUX_MAX_ISOC_XFER				32768


static pthread_t	event_thread;
static char				device_dir[PATH_MAX + 1] = "";
static GMainLoop	*event_loop;
static int32_t		linux_backend_inited = 0;



/*
 * translate_errno
 *
 *  Translates errno codes into the appropriate OPENUSB error code
 */
int32_t translate_errno(int errnum)
{
	switch(errnum)
	{
		default:
			return (OPENUSB_SYS_FUNC_FAILURE);

		case EPERM:
			return (OPENUSB_INVALID_PERM);

		case EINVAL:
			return (OPENUSB_BADARG);

		case ENOMEM:
			return (OPENUSB_NO_RESOURCES);

		case EACCES:
			return (OPENUSB_NOACCESS);

		case EBUSY:
			return (OPENUSB_BUSY);

		case EPIPE:
			return (OPENUSB_IO_STALL);

		case ENODEV:
			return (OPENUSB_UNKNOWN_DEVICE);
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
		return OPENUSB_BADARG;

	fd = open(idev->sys_path, O_RDWR);
	if (fd < 0) {
		fd = open(idev->sys_path, O_RDONLY);
		if (fd < 0) {
			usbi_debug(NULL, 1, "failed to open %s: %s", idev->sys_path,
								 strerror(errno));
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
		return (OPENUSB_BADARG);
	}

	/* allocate memory for our private data */
	hdev->priv = calloc(sizeof(struct usbi_dev_hdl_private), 1);
	if (!hdev->priv) {
		return (OPENUSB_NO_RESOURCES);
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
		return (OPENUSB_NO_RESOURCES);
	}

	/* usbfs has set the configuration to 0, so make sure we note that */
	hdev->idev->cur_config = 0;
	hdev->config_value = 1;

	return (OPENUSB_SUCCESS);
}



/*
 * linux_close
 *
 *  Close the device and return it to it's original state
 */
int32_t linux_close(struct usbi_dev_handle *hdev)
{
	/* Validate... */
	if (!hdev)
		return (OPENUSB_BADARG);

	pthread_mutex_lock(&hdev->lock);
	hdev->state = USBI_DEVICE_CLOSING;
	pthread_mutex_unlock(&hdev->lock);

	/* Stop the IO processing (polling) thread */
	wakeup_io_thread(hdev);
	pthread_join(hdev->priv->io_thread, NULL);
	
	/* close the event pipes */
	if (hdev->priv->event_pipe[0] > 0)
		close(hdev->priv->event_pipe[0]);
	if (hdev->priv->event_pipe[1] > 0)
		close(hdev->priv->event_pipe[1]);
	
	/* If we've already closed the file, we're done */
	if (hdev->priv->fd <= 0) {
		free(hdev->priv);
		return (OPENUSB_SUCCESS);
	}

	pthread_mutex_lock(&hdev->lock);
	if (close(hdev->priv->fd) == -1) {
		/* Log the fact that we had a problem closing the file, however failing a
		 * close isn't really an error, so return success anyway */
		usbi_debug(hdev->lib_hdl, 2, "error closing device fd %d: %s",
							 hdev->priv->fd, strerror(errno));
	}
	pthread_mutex_unlock(&hdev->lock);

	/* free our private data */
	free(hdev->priv);

	return (OPENUSB_SUCCESS);
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
		return OPENUSB_BADARG;

	ret = ioctl(hdev->priv->fd, IOCTL_USB_SETCONFIG, &cfg);
	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1, "could not set config %u: %s", cfg,
							 strerror(errno));
		return (translate_errno(errno));
	}

	hdev->idev->cur_config = cfg;
	hdev->config_value = cfg + 1;

	return (OPENUSB_SUCCESS);
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
		return OPENUSB_BADARG;

	*cfg = hdev->idev->cur_config;

	return OPENUSB_SUCCESS;
}



/*
 * linux_claim_interface
 *
 *  Claims the USB Interface, via IOCTL_USB_CLAIMINTF
 */
int32_t linux_claim_interface(struct usbi_dev_handle *hdev, uint8_t ifc,
                               openusb_init_flag_t flags)
{
	int32_t ret;
	int		interface = (int)ifc;

  /* Validate... */
	if (!hdev) {
		return (OPENUSB_BADARG);
	}

	/* keep track of the fact that this interface was claimed */
	if (hdev->claimed_ifs[ifc].clm == USBI_IFC_CLAIMED) {
		return OPENUSB_SUCCESS;
	}

	usbi_debug(hdev->lib_hdl, 2, "claiming interface %d", ifc);
	ret = ioctl(hdev->priv->fd, IOCTL_USB_CLAIMINTF, &interface);
	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1, "could not claim interface %d: %s", ifc,
							 strerror(errno));

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
										 "could not claim interface %d, after "
										 "detaching kernel driver: %s", ifc, openusb_strerror(ret));
					ret = linux_attach_kernel_driver(hdev,ifc);
					if (ret < 0) {
						usbi_debug(hdev->lib_hdl, 1, "could not reattach kernel driver: %s",
											 openusb_strerror(ret));
						return (ret);
					}
				}
			} else {
				usbi_debug(hdev->lib_hdl, 1, "could not detach kernel driver: %s",
									 openusb_strerror(ret));
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
	return (ret);
}



/*
 * linux_release_interface
 * 
 *  Releases the specified interface, via IOCTL_USB_RELEASEINTF
 */
int32_t linux_release_interface(struct usbi_dev_handle *hdev, uint8_t ifc)
{
	int32_t ret;
	int			interface = (int)ifc;
	
	/* Validate... */
	if (!hdev) {
		return OPENUSB_BADARG;
	}

	ret = ioctl(hdev->priv->fd, IOCTL_USB_RELEASEINTF, &interface);
	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1, "could not release interface %d: %s", ifc,
							 strerror(errno));
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

	return (OPENUSB_SUCCESS);
}



/*
 * linux_set_altsetting
 *
 *  Sets the alternate setting, via IOCTL_USB_SETINTF
 */
int32_t linux_set_altsetting(struct usbi_dev_handle *hdev, uint8_t ifc,
														 uint8_t alt)
{
	struct usbk_setinterface setintf;
	int32_t                  ret;

	/* Validate... */
	if (!hdev)
		return OPENUSB_BADARG;

	if (hdev->claimed_ifs[ifc].clm != USBI_IFC_CLAIMED) {
		usbi_debug(hdev->lib_hdl, 1, "interface (%d) must be claimed before "
							 "assigning an alternate setting", ifc);
		return OPENUSB_BADARG;
	}

	/* fill in the kernel structure for our IOCTL */
	setintf.interface   = ifc;
	setintf.altsetting  = alt;

	/* send the IOCTL */
	ret = ioctl(hdev->priv->fd, IOCTL_USB_SETINTF, &setintf);
	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1,
							 "could not set alternate setting for %s, ifc = %d, alt = %d: %s",
							 hdev->idev->sys_path, ifc, alt, strerror(errno));
		return (translate_errno(errno));
	}

	/* keep track of the alternate setting */
	hdev->claimed_ifs[ifc].altsetting = alt;

	return (OPENUSB_SUCCESS);
}



/*
 * linux_get_altsetting
 *
 *  Gets the alternate setting from our cached value. There is no usbdevfs IOCTL
 *  to do this so this is the best we can do.
 */
int32_t linux_get_altsetting(struct usbi_dev_handle *hdev, uint8_t ifc,
														 uint8_t *alt)
{
	/* Validate... */
	if ((!hdev) || (!alt))
		return (OPENUSB_BADARG);

	*alt = hdev->claimed_ifs[ifc].altsetting;

	return (OPENUSB_SUCCESS);
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
		return (OPENUSB_BADARG);

	ret = ioctl(hdev->priv->fd, IOCTL_USB_RESET, NULL);
	if (ret) {
		usbi_debug(hdev->lib_hdl, 1, "could not reset: %s", strerror(errno));
		return translate_errno(errno);
	}

	return OPENUSB_SUCCESS;
}



/*
 * linux_clear_halt
 *
 *  Clear halted endpoint, for backward compatibility with openusb 0.1
 */
int32_t linux_clear_halt(struct usbi_dev_handle *hdev, uint8_t ept)
{
	int32_t ret;

	/* Validate... */
	if (!hdev)
		return (OPENUSB_BADARG);

	ret = ioctl(hdev->priv->fd, IOCTL_USB_CLEAR_HALT, &ept);
	if (ret) {
		usbi_debug(hdev->lib_hdl, 1, "could not clear halt ep %d: %s", ept,
							 strerror(errno));
		return translate_errno(errno);
	}

	return OPENUSB_SUCCESS;
}



/* linux_init
 *
 *  Backend initialization, called in openusb_init()
 *    flags - inherited from openusb_init(), TBD
 */
int32_t linux_init(struct usbi_handle *hdl, uint32_t flags )
{
	int32_t 				ret;

	/* Validate... */
	if (!hdl)
		return (OPENUSB_BADARG);

	if(linux_backend_inited != 0) {
		/*already inited */
		usbi_debug(hdl, 1, "Linux backend already initialized");
		linux_backend_inited++; /* openusb_init gets called once more */
		return (OPENUSB_SUCCESS);
	}

	/* Find the path to the directory tree with the device nodes */
	if (getenv("USB_PATH")) {
		if (check_usb_path(getenv("USB_PATH"))) {
			strncpy(device_dir, getenv("USB_PATH"), sizeof(device_dir) - 1);
			device_dir[sizeof(device_dir) - 1] = 0;
		} else
			usbi_debug(hdl, 1, "couldn't find USB devices in USB_PATH");
	}

	if (!device_dir[0]) {
		if (check_usb_path("/dev/bus/usb")) {
			strncpy(device_dir, "/dev/bus/usb", sizeof(device_dir) - 1);
			device_dir[sizeof(device_dir) - 1] = 0;
		} else if (check_usb_path("/proc/bus/usb")) {
			strncpy(device_dir, "/proc/bus/usb", sizeof(device_dir) - 1);
			device_dir[sizeof(device_dir) - 1] = 0;
		} else {
			device_dir[0] = 0;  /* No path, no USB support */
		}
	}

	if (device_dir[0]) {
		usbi_debug(hdl, 1, "found USB device directory at %s", device_dir);
	} else {
		usbi_debug(hdl, 1, "no USB device directory found");
	}

	/* Initialize thread support in GLib (if it's not already) */
	if (!g_thread_supported()) g_thread_init(NULL);
	
	/* Start up thread for polling events */
	ret = pthread_create(&event_thread, NULL, hal_hotplug_event_thread,
												(void*)NULL);
	if (ret < 0) {
		usbi_debug(NULL, 1, "unable to create event polling thread: %d)", ret);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	/* we're initialized */
	linux_backend_inited++;

	return (OPENUSB_SUCCESS);
}



/*
 * linux_fini
 *
 *  Backend specific data cleanup, called in openusb_fini()
 */
void linux_fini(struct usbi_handle *hdl)
{
	
	/* If we're not initailized, don't bother */
	if (!linux_backend_inited) {
		return;
	}

	/* If this is not our last instance, decrement the count and leave */
	if (linux_backend_inited > 1) {
		linux_backend_inited--;
		return;
	}

	/* If we're here this is our last instance */
	/* Stop the event (connect/disconnect) thread */
	if ((event_loop != NULL) && g_main_loop_is_running(event_loop)) {
		usbi_debug(hdl, 4, "stopping the hotplug thread...");
		g_main_loop_quit(event_loop);
		g_main_context_wakeup(g_main_loop_get_context(event_loop));
		pthread_join(event_thread, NULL);
	}
	
	/* We're no longer initialized */
	linux_backend_inited--;

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
	openusb_busid_t busnum;


	/* Validate... */
	if (!buses) {
		return (OPENUSB_BADARG);
	}

	/* Scan our device directory for bus directories */
	dir = opendir(device_dir);
	if (!dir) {
		usbi_debug(NULL, 1, "could not opendir(%s): %s", device_dir,
							 strerror(errno));
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
			return (OPENUSB_NO_RESOURCES);
		}

		memset(ibus, 0, sizeof(*ibus));
		ibus->priv = (struct usbi_bus_private *)
				calloc(sizeof(struct usbi_bus_private), 1);
		if (!ibus->priv) {
			free(ibus);
			usbi_debug(NULL,1, "malloc ibus private failed: %s",strerror(errno));
			return (OPENUSB_NO_RESOURCES);
		}

		/* setup the maximum transfer sizes - these are determined by usbfs and
		 * NOT by the bus (as with other oses) so they will always be the same */
		ibus->max_xfer_size[USB_TYPE_CONTROL]     = 4096 - USBI_CONTROL_SETUP_LEN;
		ibus->max_xfer_size[USB_TYPE_INTERRUPT]   = pow(2,32) - 1;
		ibus->max_xfer_size[USB_TYPE_BULK]        = pow(2,32) - 1;
		ibus->max_xfer_size[USB_TYPE_ISOCHRONOUS] = pow(2,32) - 1;

		pthread_mutex_init(&ibus->lock, NULL);
		pthread_mutex_init(&ibus->devices.lock, NULL);

		ibus->busnum = atoi(entry->d_name);
		snprintf(ibus->sys_path, sizeof(ibus->sys_path), "%s/%s",
						 device_dir, entry->d_name);
		list_add(&ibus->list, buses);
		usbi_debug(NULL, 3, "found bus dir %s", ibus->sys_path);
	}

	closedir(dir);

	return (OPENUSB_SUCCESS);
}



/*
 * linux_free_device
 *
 *  Cleanup backend specific data in the usbi_device structure. Called when the
 *  device node is to be removed from the device list.
 */
void linux_free_device(struct usbi_device *idev)
{
	/* Free the udi and the private data structure */
	if (idev->priv) {
		if (idev->priv->udi) {
			free(idev->priv->udi);
			idev->priv->udi = NULL;
		}
		free(idev->priv);
		idev->priv = NULL;
	}

	return;
}



/******************************************************************************
 *                                IO Functions                                *
 *****************************************************************************/
/*
 * linux_submit_ctrl
 *
 *  Submits an io request to the control endpoint
 */
int32_t linux_submit_ctrl(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	openusb_ctrl_request_t *ctrl;
	uint8_t 							setup[USBI_CONTROL_SETUP_LEN];
	int32_t								ret;
	
	/* Validate... */
	if ((!hdev) || (!io)) {
		return (OPENUSB_BADARG);
	}

	pthread_mutex_lock(&io->lock);
	
	/* allocate memory for the private part */
	io->priv = malloc(sizeof(struct usbi_io_private));
	if (!io->priv) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the "
							 "private io member");
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv, 0, sizeof(*io->priv));

	/* allocate memory for the urb */
	io->priv->num_urbs = 1;
	io->priv->urbs = (struct usbk_urb*)malloc(sizeof(struct usbk_urb));
	if (!io->priv->urbs) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the urb");
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv->urbs, 0, sizeof(struct usbk_urb));
	
	/* get a pointer to the request */
	ctrl = io->req->req.ctrl;

	/* setup the user context */
	io->priv->urbs[0].usercontext = io;
	
	/* fill in the setup packet */
	setup[0] = ctrl->setup.bmRequestType;
	setup[1] = ctrl->setup.bRequest;
	*(uint16_t *)(setup + 2) = openusb_cpu_to_le16(ctrl->setup.wValue);
	*(uint16_t *)(setup + 4) = openusb_cpu_to_le16(ctrl->setup.wIndex);
	*(uint16_t *)(setup + 6) = openusb_cpu_to_le16(ctrl->length);
	
	/* setup the URB */
	io->priv->urbs[0].type = USBK_URB_TYPE_CONTROL;

	/* allocate a temporary buffer for the payload */
	io->priv->urbs[0].buffer = malloc(USBI_CONTROL_SETUP_LEN + ctrl->length);
	if (!io->priv->urbs[0].buffer) {
		pthread_mutex_unlock(&io->lock);
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv->urbs[0].buffer,0,USBI_CONTROL_SETUP_LEN + ctrl->length);

	/* fill in the temporary buffer */
	memcpy(io->priv->urbs[0].buffer, setup, USBI_CONTROL_SETUP_LEN);
	io->priv->urbs[0].buffer_length = USBI_CONTROL_SETUP_LEN + ctrl->length;

	/* copy the data if we're writing */
	if ((ctrl->setup.bmRequestType & USB_REQ_DIR_MASK) == USB_REQ_HOST_TO_DEV) {
		memcpy(io->priv->urbs[0].buffer + USBI_CONTROL_SETUP_LEN,
					 ctrl->payload, ctrl->length);
	}

	/* lock the device */
	pthread_mutex_lock(&hdev->lock);
	
	/* submit the URB */
	ret = urb_submit(hdev, &io->priv->urbs[0]);
	if (ret < 0) {
		usbi_debug(hdev->lib_hdl, 1, "error submitting first URB: %s",
							 strerror(errno));
		io->status = USBI_IO_COMPLETED_FAIL;
		return translate_errno(errno);
	}
	
	/* unlock the device & io request */
	pthread_mutex_unlock(&io->lock);
	pthread_mutex_unlock(&hdev->lock);

	/* always do this to avoid race conditions */
	wakeup_io_thread(hdev);
	
	return (ret);
}



/*
 * linux_submit_bulk_intr
 *
 *  Submits an io request to a bulk or interrupt endpoint
 */
int32_t linux_submit_bulk_intr(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int32_t		ret;
	int32_t		i;
	uint8_t		partial_last_urb = 0;
	uint8_t		*payload;
	uint32_t	length;
	uint8_t		xfertype;

	/* Validate... */
	if ((!hdev) || (!io)) {
		return (OPENUSB_BADARG);
	}

	pthread_mutex_lock(&io->lock);
	
	/* allocate memory for the private part */
	io->priv = malloc(sizeof(struct usbi_io_private));
	if (!io->priv) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the "
							 "private io member");
		pthread_mutex_unlock(&io->lock);
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv, 0, sizeof(*io->priv));

	/* setup the payload, length and type we need for later */
	if (io->req->type == USB_TYPE_BULK) {
		payload = io->req->req.bulk->payload;
		length	= io->req->req.bulk->length;
		xfertype= USBK_URB_TYPE_BULK;
	} else if (io->req->type == USB_TYPE_INTERRUPT) {
		payload = io->req->req.intr->payload;
		length	= io->req->req.intr->length;
		xfertype= USBK_URB_TYPE_INTERRUPT;
	} else {
		usbi_debug(hdev->lib_hdl, 1, "transfer type is not bulk or interrupt");
		return (OPENUSB_BADARG);
	}

	/* usbfs only allows transfer sizes of up to 16KB, so we'll probably need to
	 * split this request up into multiple chunks and fire them all off at once */
	io->priv->num_urbs = length / LINUX_MAX_BULK_INTR_XFER;
	if ((length % LINUX_MAX_BULK_INTR_XFER) > 0) {
		partial_last_urb = 1;
		io->priv->num_urbs++;
	}
	usbi_debug(hdev->lib_hdl, 4, "%d urbs needed for bulk/intr xfer of length %d",
						 io->priv->num_urbs, length);

	/* allocate memory for our urbs */
	io->priv->urbs = (struct usbk_urb*)malloc(  io->priv->num_urbs
																						* sizeof(struct usbk_urb));
	if (!io->priv->urbs) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for %d urbs",
							 io->priv->num_urbs);
		return (OPENUSB_NO_RESOURCES);
	}
	
	/* initialize our variables */
	memset(io->priv->urbs, 0, io->priv->num_urbs * sizeof(struct usbk_urb));
	io->priv->urbs_to_reap = 0;
	io->priv->urbs_to_cancel = 0;

	/* now setup each urb and fire it off */
	pthread_mutex_lock(&hdev->lock);
	io->status = USBI_IO_INPROGRESS;
	io->priv->reap_action = NORMAL;
	for(i = 0; i < io->priv->num_urbs; i++) {

		/* get a point to our urb for easier access */
		struct usbk_urb	*urb = &io->priv->urbs[i];

		/* setup this urb */
		urb->endpoint			= io->req->endpoint;
		urb->usercontext	= (void*)io;
		urb->type					= xfertype;
		urb->buffer				= payload + (i * LINUX_MAX_BULK_INTR_XFER);

		if ((i == io->priv->num_urbs - 1) && partial_last_urb) {
			urb->buffer_length	= length % LINUX_MAX_BULK_INTR_XFER;
		} else {
			urb->buffer_length	= LINUX_MAX_BULK_INTR_XFER;
		}
	
		/* submit the urb */
		ret = urb_submit(hdev, urb);
		if (ret < 0) {

			/* if this is the first URB we've submitted, things are simple */
			if (i == 0) {
				usbi_debug(hdev->lib_hdl, 1, "error submitting first URB: %s",
									 strerror(errno));
				io->status = USBI_IO_COMPLETED_FAIL;
				return translate_errno(errno);
			}

			/* if it's not the first urb then the logic gets more complicated */
			handle_partial_submit(hdev, io, i);
			return (OPENUSB_SUCCESS);
		}

	} /* end for(i = 0; i < io->priv->num_urbs; i++) */

	/* unlock the device & io request */
	pthread_mutex_unlock(&io->lock);
	pthread_mutex_unlock(&hdev->lock);

	/* always do this to avoid race conditions */
	wakeup_io_thread(hdev);
	
	return (OPENUSB_SUCCESS);
}



/*
 * linux_submit_isoc
 *
 *  Submits an isochronous io request
 */
int32_t linux_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	openusb_isoc_request_t	*isoc;
	int32_t									ret;
	int32_t									i, this_urb_len, packet_offset;
	uint32_t								packet_len;
	struct usbk_urb					*urb;
	int32_t									space_remaining_in_urb;
	int32_t									urb_packet_offset;
	int32_t									j,k;
	uint8_t									*urb_buffer;
	
	if((!io) || (!hdev)) {
		return (OPENUSB_BADARG);
	}

	pthread_mutex_lock(&io->lock);

	/* intialize */
	this_urb_len = 0;
	packet_offset = 0;
	io->priv->num_urbs = 0;
	
	/* allocate memory for the private part */
	io->priv = malloc(sizeof(struct usbi_io_private));
	if (!io->priv) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for the "
							 "private io member");
		pthread_mutex_unlock(&io->lock);
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv, 0, sizeof(*io->priv));

	/* get a pointer to our request (for easier access) */
	isoc = io->req->req.isoc;
	
	/* usbfs only allows transfer sizes of up to 32KB, so we'll probably need to
	 * split this request up into multiple chunks and fire them all off at once */
	for (i = 0; i < isoc->pkts.num_packets; i++) {
		space_remaining_in_urb = LINUX_MAX_ISOC_XFER - this_urb_len;
		packet_len = isoc->pkts.packets[i].length;

		if (packet_len > space_remaining_in_urb) {
			io->priv->num_urbs++;
			this_urb_len = packet_len;
		} else {
			this_urb_len += packet_len;
		}
	}
	usbi_debug(hdev->lib_hdl, 4, "%d URBs needed for isoc transfer",
						 io->priv->num_urbs);

	/* allocate memory for our array of urbs */
	io->priv->iso_urbs = (struct usbk_urb**)malloc(  io->priv->num_urbs
																								 * sizeof(struct usbk_urb));
	if(!io->priv->iso_urbs) {
		usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for %d urbs",
							 io->priv->num_urbs);
		return (OPENUSB_NO_RESOURCES);
	}
	memset(io->priv->iso_urbs, 0, io->priv->num_urbs * sizeof(struct usbk_urb));

	io->priv->urbs_to_cancel			= 0;
	io->priv->urbs_to_reap				= 0;
	io->priv->reap_action					= NORMAL;
	io->priv->isoc_packet_offset	= 0;

	/* allocate and initialize each urb with the correct number of packets */
	for (i = 0; i < io->priv->num_urbs; i++) {

		space_remaining_in_urb = LINUX_MAX_ISOC_XFER;
		urb_packet_offset = 0;

		/* get all of the packets that will fit in this urb */
		while (packet_offset < isoc->pkts.num_packets) {
			packet_len = isoc->pkts.packets[packet_offset].length;
			if (packet_len <= space_remaining_in_urb) {
				/* include this packet */
				urb_packet_offset++;
				packet_offset++;
				space_remaining_in_urb -= packet_len;
				urb->buffer_length += packet_len;
			} else {
				/* it won't fit, put it in the next urb */
				break;
			}
		}

		/* allocate memory for this specific urb */
		urb = (struct usbk_urb*)malloc(  sizeof(*urb)
																	 + (  urb_packet_offset
																			* sizeof(struct usbk_iso_packet_desc)) );
		if (!urb) {
			free_isoc_urbs(io);
			return (OPENUSB_NO_RESOURCES);
		}
		memset(urb, 0,  sizeof(*urb)
									+ (urb_packet_offset * sizeof(struct usbk_iso_packet_desc)));
		io->priv->iso_urbs[i] = urb;

		/* allocate memory for the urb buffer */
		urb->buffer = (void*)malloc(urb->buffer_length);
		if (!urb->buffer) {
			usbi_debug(hdev->lib_hdl, 1, "unable to allocate memory for urb buffer "
								 "of length %d", urb->buffer_length);
			free_isoc_urbs(io);
			return (OPENUSB_NO_RESOURCES);
		}
		memset(urb->buffer, 0, urb->buffer_length);
		urb_buffer = urb->buffer;
		
		/* setup the packet lengths and copy in the data */
		for (j=0, k=packet_offset-urb_packet_offset; k<packet_offset; k++, j++) {
			packet_len = isoc->pkts.packets[k].length;
			urb->iso_frame_desc[j].length = packet_len;
			if ((io->req->endpoint & USB_REQ_DIR_MASK) == USB_REQ_HOST_TO_DEV) {
				memcpy(urb->buffer, isoc->pkts.packets[k].payload, packet_len);
			}
			urb_buffer += packet_len;
		}

		urb->usercontext	= io;
		urb->type					= USBK_URB_TYPE_ISO;
		urb->flags				= USBK_URB_ISO_ASAP;
		urb->endpoint			= io->req->endpoint;
		urb->number_of_packets = urb_packet_offset;

	}

	/* now setup each urb and fire it off */
	pthread_mutex_lock(&hdev->lock);
	io->status = USBI_IO_INPROGRESS;
	io->priv->reap_action = NORMAL;
	for(i = 0; i < io->priv->num_urbs; i++) {

		/* get a point to our urb for easier access */
		struct usbk_urb	*urb = io->priv->iso_urbs[i];

		/* submit the urb */
		ret = urb_submit(hdev, urb);
		if (ret < 0) {

			/* if this is the first URB we've submitted, things are simple */
			if (i == 0) {
				usbi_debug(hdev->lib_hdl, 1, "error submitting first URB: %s",
									 strerror(errno));
				io->status = USBI_IO_COMPLETED_FAIL;
				return translate_errno(errno);
			}

			/* if it's not the first urb then the logic gets more complicated */
			handle_partial_submit(hdev, io, i);
			return (OPENUSB_SUCCESS);
		}

	} /* end for(i = 0; i < io->priv->num_urbs; i++) */

	/* unlock the device & io request */
	pthread_mutex_unlock(&io->lock);
	pthread_mutex_unlock(&hdev->lock);

	/* always do this to avoid race conditions */
	wakeup_io_thread(hdev);
	
	return (OPENUSB_SUCCESS);
}



/*
 * urb_submit
 *
 *	Submit the specified urb using IOCTL_USB_SUBMITURB
 */
int32_t urb_submit(struct usbi_dev_handle *hdev, struct usbk_urb *urb)
{
	return (ioctl(hdev->priv->fd, IOCTL_USB_SUBMITURB, urb));
}



/*
 * free_isoc_urbs
 *
 *	Free all the memory allocated for the isochronous urbs store in the private
 *	section of usbi_io.
 */
void free_isoc_urbs(struct usbi_io *io)
{
	int32_t 				i;
	struct usbk_urb	*urb;

	for (i = 0; i < io->priv->num_urbs; i++) {
		urb = io->priv->iso_urbs[i];
		if (!urb) {
			break;
		}
		free(urb->buffer);
		free(urb);
	}

	free(io->priv->iso_urbs);
	return;
}



/*
 * discard_urbs
 *
 *	This functions uses IOCTL_DISCARDURB to discard all the URBs for a given
 *	usbi_io request. It also tracks how many URBs we expected to be handled
 */
void discard_urbs(struct usbi_dev_handle *hdev, struct usbi_io *io,
									linux_reap_action_t reap_action)
{
	int32_t	i, ret;

	io->priv->reap_action = reap_action;
	for (i = 0; i < io->priv->num_urbs; i++) {
		ret = ioctl(hdev->priv->fd, IOCTL_USB_DISCARDURB, &io->priv->urbs[i]);
		if (ret == 0) {
			io->priv->urbs_to_cancel++;
		} else if (errno == EINVAL) {
			io->priv->urbs_to_reap++;
		} else {
			usbi_debug(hdev->lib_hdl, 4, "failed to cancel URB %d: %s", errno,
								 strerror(errno));
		}
	}

	return;
}



/*
 * handle_partial_submit
 *
 *	This functions takes care of discarding all previously submitted URBs for a
 *	give usbi_io request in case IOCTL_USB_SUBMITURB fails sometime after the
 *	first URB in the io request fails.
 */
void handle_partial_submit(struct usbi_dev_handle *hdev, struct usbi_io *io,
													 int32_t idx)
{
	int32_t	i, ret;

	/* This function is called when submitting a set of urbs to usbfs and
	* an urb that is not the first one fails. We must cancel all of the URBs
	* that have been submitted up to this point.
	*
	* Since cancelling the urbs is asynchronous they will still be reaped
	* later. So we want to return success even though we know we have a
	* problem to hopefully prevent the user from freeing any memory we'll
	* need when reaping the canceled urbs later.
	*
	* Also, some of the URBs we're about to cancel may have succeeded and
	* we don't want to throw away that data, so we'll set the reap action
	* such that it will save data from those that completed successfully.
	*/
	io->priv->reap_action = SUBMIT_FAILED;
	for (i = 0; i < idx; i++) {
		ret = ioctl(hdev->priv->fd, IOCTL_USB_DISCARDURB, &io->priv->urbs[i]);
		if (ret == 0) {
			io->priv->urbs_to_cancel++;
		} else if (errno == EINVAL) {
			io->priv->urbs_to_reap++;
		} else {
			usbi_debug(hdev->lib_hdl, 4, "failed to cancel URB %d: %s", errno,
								 strerror(errno));
		}
	}

	usbi_debug(hdev->lib_hdl, 1, "some urbs failed to submit, reporting success "
			"but waiting for %d cancels and %d reaps before reporting an error",
			io->priv->urbs_to_cancel, io->priv->urbs_to_reap);
	return;
}



/*
 * io_complete
 *
 *  This function is called by the poll_io thread when a submitted io request
 *  has been completed.
 */
int32_t io_complete(struct usbi_dev_handle *hdev)
{
	struct usbk_urb		*urb	= NULL;
	struct usbi_io		*io		= NULL;


	while(ioctl(hdev->priv->fd, IOCTL_USB_REAPURBNDELAY, (void*)&urb) >= 0) {

		io = urb->usercontext;

		/* We handle the completion of bulk, interrupt and control requests
		 * differently than we handle the completion of isochronous requests */
		switch (io->req->type) {
			default:
				usbi_debug(hdev->lib_hdl, 1, "unrecognized usb transfer type: %d",
									 io->req->type);
				break;

			case USB_TYPE_CONTROL:
				
				if (urb->status == 0) {
					/* copy the data back */
					memcpy(io->req->req.ctrl->payload,
								 urb->buffer + USBI_CONTROL_SETUP_LEN,
								 io->req->req.ctrl->length);
					io->status = USBI_IO_COMPLETED;
					usbi_io_complete(io, OPENUSB_SUCCESS, urb->actual_length);
				}
			
				/* report successful completion */
				if (urb->status == -ENOENT) {
					if (io->priv->reap_action == CANCELED) {
						io->status = USBI_IO_CANCEL;
						usbi_io_complete(io, OPENUSB_IO_CANCELED, urb->actual_length);
					} else if (io->priv->reap_action == TIMEDOUT) {
						io->status = USBI_IO_TIMEOUT;
						usbi_io_complete(io, OPENUSB_IO_TIMEOUT, urb->actual_length);
					} else {
						io->status = USBI_IO_COMPLETED_FAIL;
						usbi_io_complete(io, OPENUSB_SYS_FUNC_FAILURE, urb->actual_length);
					}
				}

				free(urb->buffer);
				free(io->priv->urbs);
				break;

			case USB_TYPE_BULK:
			case USB_TYPE_INTERRUPT:
				handle_bulk_intr_complete(hdev, urb);
				break;

			case USB_TYPE_ISOCHRONOUS:
				handle_isoc_complete(hdev, urb);
				break;
		}
	}

	return (OPENUSB_SUCCESS);
}



/*
 * handle_bulk_intr_complete
 *
 *	This function handles keeping track of the URBs that have been completed for
 *	for a give usbi_io request. It handles error statuses and keeping track of
 *	how many URBs remain for a specific request so that timeouts, canceles and
 *	early completions are handled properly.
 */
int32_t handle_bulk_intr_complete(struct usbi_dev_handle *hdev,
																	struct usbk_urb *urb)
{
	struct usbi_io	*io;
	int32_t					urb_index;

	/* Get the pointer to our io */
	io = urb->usercontext;

	/* Get the index of this urb */
	urb_index = urb - io->priv->urbs;

	/* spit out something useful... */
	usbi_debug(hdev->lib_hdl, 4, "processing urb %d/%d: status = %d",
						 urb_index+1, io->priv->num_urbs, urb->status);
	
	/* keep track of the bytes transferred */
	if (urb->status == 0) {
		io->priv->bytes_transferred += urb->actual_length;
	}

	/* if the reap action is not normal we have some special handling to do */
	if (io->priv->reap_action != NORMAL) {

		if (urb->status == -ENOENT) {
			usbi_debug(hdev->lib_hdl, 4, "canceled urb found");
			if (io->priv->urbs_to_cancel == 0) {
				usbi_debug(hdev->lib_hdl, 1, "canceled urb found, but no urbs "
									 " have been canceled!");
			} else {
				io->priv->urbs_to_cancel--;
			}
		} else if (urb->status == 0) {
			usbi_debug(hdev->lib_hdl, 4, "completed urb found");

			if (io->priv->reap_action == COMPLETED_EARLY) {
				usbi_debug(hdev->lib_hdl, 1, "WARNING SOME DATA WAS LOST (completed "
									 "early but a remaining urb also completed): ep = %x",
									 io->req->endpoint);
			}

			if (io->priv->urbs_to_reap == 0) {
				usbi_debug(hdev->lib_hdl, 1,
									 "completed URB but not awaiting a completion");
			} else {
				io->priv->urbs_to_reap--;
			}

		} else {
			usbi_debug(hdev->lib_hdl, 2,
								 "unrecognized urb status (on cancel): %d", urb->status);
		}

		if ((io->priv->urbs_to_reap == 0) && (io->priv->urbs_to_cancel == 0)) {

			usbi_debug(hdev->lib_hdl, 4, "last URB handled, io request complete");
			if (io->priv->reap_action == CANCELED) {
				usbi_io_complete(io, OPENUSB_IO_CANCELED, io->priv->bytes_transferred);
				free(io->priv->urbs);
				return (OPENUSB_SUCCESS);
			} else if (io->priv->reap_action == COMPLETED_EARLY) {
				usbi_io_complete(io, OPENUSB_SUCCESS, io->priv->bytes_transferred);
				free(io->priv->urbs);
				return (OPENUSB_SUCCESS);
			} else if (io->priv->reap_action == TIMEDOUT) {
				usbi_io_complete(io, OPENUSB_IO_TIMEOUT, io->priv->bytes_transferred);
				free(io->priv->urbs);
				return (OPENUSB_SUCCESS);
			} else {
				usbi_io_complete(io, OPENUSB_SYS_FUNC_FAILURE,
												 io->priv->bytes_transferred);
				free(io->priv->urbs);
				return (OPENUSB_SUCCESS);
			}
		}

		return (OPENUSB_SUCCESS);
	}

	/* check for errors */
	if (urb->status == -EPIPE) {
		usbi_debug(hdev->lib_hdl, 1, "endpoint %x stalled", io->req->endpoint);
		free(io->priv->urbs);
		usbi_io_complete(io, OPENUSB_IO_STALL, urb->actual_length);
	} else if (urb->status != 0) {
		usbi_debug(hdev->lib_hdl, 1, "unrecognized urb status: %d", urb->status);
	}

	/* if this is the last urb we need to complete or we received less data than
	* requested we're done */
	if (urb_index == io->priv->num_urbs - 1) {

		usbi_debug(hdev->lib_hdl, 4, "last URB in transfer, io request complete");
		usbi_io_complete(io, OPENUSB_SUCCESS, io->priv->bytes_transferred);
		free(io->priv->urbs);
		return (OPENUSB_SUCCESS);

	} else if (urb->actual_length < urb->buffer_length) {

		usbi_debug(hdev->lib_hdl, 4, "short transfer on ep %x, urb %d/%d, total %d",
							 io->req->endpoint, urb->actual_length, urb->buffer_length,
							 io->priv->bytes_transferred);

		/* cancel the remaining urbs */
		handle_partial_xfer(hdev, io, urb_index + 1);

		return (OPENUSB_SUCCESS);
	}

	return (OPENUSB_SUCCESS);
}



/*
 * handle_isoc_complete
 *
 *	This function handles keeping track of the URBs that have been completed for
 *	for a give usbi_io request. It handles error statuses and keeping track of
 *	how many URBs remain for a specific request so that canceles are handled
 *	properly. This function only handle isochronous URBs).
 */
int32_t handle_isoc_complete(struct usbi_dev_handle *hdev, struct usbk_urb *urb)
{
	struct usbi_io	*io;
	int32_t					urb_index;
	int32_t					i;
	uint8_t					*urb_buffer;

	openusb_request_result_t	*isoc_results;
	openusb_isoc_request_t		*isoc;

	/* Initialization */
	io = urb->usercontext;
	urb_index = 0;

	for (i = 0; i < io->priv->num_urbs; i++) {
		if (urb == io->priv->iso_urbs[i]) {
			urb_index = i + 1;
			break;
		}
	}
	if (urb_index == 0) {
		usbi_debug(hdev->lib_hdl, 1, "failed to find urb (isoc xfer)");
		return (OPENUSB_BADARG);
	}

	usbi_debug(hdev->lib_hdl, 4, "handling completion of iso urb %d/%d: %d",
						 urb_index, io->priv->num_urbs, urb->status);

	if (urb->status == 0) {

		/* copy the isochronous results back in */
		urb_buffer = urb->buffer;
		isoc = io->req->req.isoc;
		isoc_results = isoc->isoc_results;
		for (i = 0; i < urb->number_of_packets; i++) {
			isoc_results[io->priv->isoc_packet_offset].status =
					translate_errno(-urb->iso_frame_desc[i].status);
			isoc_results[io->priv->isoc_packet_offset].transferred_bytes =
					urb->iso_frame_desc[i].actual_length;
			if ((io->req->endpoint & USB_REQ_DIR_MASK) == USB_REQ_DEV_TO_HOST) {
				memcpy(isoc->pkts.packets[io->priv->isoc_packet_offset].payload,
							 urb_buffer, urb->iso_frame_desc[i].actual_length);
				urb_buffer += urb->iso_frame_desc[i].actual_length;
			}
			io->priv->bytes_transferred += urb->iso_frame_desc[i].actual_length;
			io->priv->isoc_packet_offset++;
		}
	}

	if (io->priv->reap_action != NORMAL) {
	
		if (urb->status == -ENOENT) {
			usbi_debug(hdev->lib_hdl, 4, "canceled urb found");
			if (io->priv->urbs_to_cancel == 0) {
				usbi_debug(hdev->lib_hdl, 1,
									 "canceled urb found, but no urbs have been canceled!");
			} else {
				io->priv->urbs_to_cancel--;
			}
		} else if (urb->status == 0) {
			usbi_debug(hdev->lib_hdl, 4, "completed urb found");

			if (io->priv->urbs_to_reap == 0) {
				usbi_debug(hdev->lib_hdl, 1,
									 "completed URB but not awaiting a completion");
			} else {
				io->priv->urbs_to_reap--;
			}

		} else {
			usbi_debug(hdev->lib_hdl, 2, "unrecognized urb status (on cancel): %d",
								 urb->status);
		}

		if ((io->priv->urbs_to_reap == 0) && (io->priv->urbs_to_cancel == 0)) {

			usbi_debug(hdev->lib_hdl, 4, "last URB handled, io request complete");
			if (io->priv->reap_action == CANCELED) {
				usbi_io_complete(io, OPENUSB_IO_CANCELED, io->priv->bytes_transferred);
				free_isoc_urbs(io);
				return (OPENUSB_SUCCESS);
			} else {
				usbi_io_complete(io, OPENUSB_SYS_FUNC_FAILURE,
												 io->priv->bytes_transferred);
				free_isoc_urbs(io);
				return (OPENUSB_SUCCESS);
			}
		}

		return (OPENUSB_SUCCESS);
	}
	
	if (urb->status != 0) {
		usbi_debug(hdev->lib_hdl, 2, "unrecognized urb status %d", urb->status);
	}

	/* if this is the last urb then we're done */
	if (urb_index == io->priv->num_urbs) {
		usbi_debug(hdev->lib_hdl, 4, "last URB in transfer completed");
		free_isoc_urbs(io);
		usbi_io_complete(io, OPENUSB_SUCCESS, io->priv->bytes_transferred);
	}

	return (OPENUSB_SUCCESS);
}



/*
 * handle_partial_xfer
 *
 *	This function handle cancelling all the remaining URBs in a give usbi_io
 *	request in the case that a transfer completes early.
 */
void handle_partial_xfer(struct usbi_dev_handle *hdev, struct usbi_io *io,
												 int32_t idx)
{
	int32_t	i, ret;

	io->priv->reap_action = COMPLETED_EARLY;
	for (i = idx; i < io->priv->num_urbs; i++) {
		ret = ioctl(hdev->priv->fd, IOCTL_USB_DISCARDURB, &io->priv->urbs[i]);
		if (ret == 0) {
			io->priv->urbs_to_cancel++;
		} else if (errno == EINVAL) {
			io->priv->urbs_to_reap++;
		} else {
			usbi_debug(NULL, 4, "failed to cancel URB %d: %s", errno,
								 strerror(errno));
		}
	}

	usbi_debug(NULL, 4,
						 "partial xfer: waiting on %d cancels and %d reaps before reporting"
						 " an error", io->priv->urbs_to_cancel, io->priv->urbs_to_reap);
	return;
}



/*
 * io_timeout
 *
 *  This function is called by the poll_io thread when a submitted io request
 *  has timed out.
 */
int32_t io_timeout(struct usbi_dev_handle *hdev, struct timeval *tvc)
{
	struct usbi_io	*io, *tio;

	/* check each entry in the io list to find out if it's timed out */
	list_for_each_entry_safe(io, tio, &hdev->io_head, list) {

		/* currently, isochronous io doesn't consider timeout issue and we don't
		 * want to process any requests that aren't in progress */
		if( (io->req->type == USB_TYPE_ISOCHRONOUS) ||
				(io->status != USBI_IO_INPROGRESS) ) {
				continue;
		}

		if (usbi_timeval_compare(&io->tvo, tvc) <= 0) {
			discard_urbs(hdev, io, TIMEDOUT);
		}
	}

	return (OPENUSB_SUCCESS);
}



/*
 * linux_io_cancel
 *
 *  Called to cancel a pending io request. This function will fail if the io
 *  request has already been submitted to the usbfs kernel driver, even if it
 *  hasn't been completed.
 *
 */
int32_t linux_io_cancel(struct usbi_io *io)
{

	io->status = USBI_IO_CANCEL;
	
	/* Discard/Cancel all the URBs for this io request */
	discard_urbs(io->dev, io, CANCELED);
	
	/* Always do this to avoid race conditions */
	wakeup_io_thread(io->dev);

	return (OPENUSB_SUCCESS);
}



/******************************************************************************
 *                             Thread Functions                               *
 *****************************************************************************/

/*
 * poll_io
 *
 *  This is our worker thread. It's a forever loop, so it must be shutdown with
 *  pthread_cancel (currently done in linux_close).
 */
void *poll_io(void *devhdl)
{
	struct usbi_dev_handle  *hdev = (struct usbi_dev_handle*)devhdl;
	struct timeval					tvc, tvo, tvNext;
	fd_set									readfds, writefds;
	int											ret, maxfd;
	struct usbi_io					*io;
	uint8_t 								buf[16];
	
	/*
	 * Loop forever checking to see if we have io requests that need to be
	 * processed and process them.
	 */
	while (1) {

		/* find out if there is any reading/writing to be done */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		/* We need to check our private event pipe, our device's file, and the
		 * event pipe used by the frontend's timeout thread (which the Linux
		 * backened doesn't use). */
		pthread_mutex_lock(&hdev->lock);
		FD_SET(hdev->priv->event_pipe[0], &readfds);
		FD_SET(hdev->event_pipe[0], &readfds);
		FD_SET(hdev->priv->fd, &writefds);

		/* get the max file descriptor for select() */
		if (hdev->priv->event_pipe[0] > hdev->priv->fd) {
			maxfd = hdev->priv->event_pipe[0];
		} else {
			maxfd = hdev->priv->fd;
		}
		if (hdev->event_pipe[0] > maxfd) {
			maxfd = hdev->event_pipe[0];
		}

		/* get the time so that we can determine if any timeouts have passed */
		gettimeofday(&tvc, NULL);
		memset(&tvo, 0, sizeof(tvo));
		memset(&tvNext, 0, sizeof(tvNext));

		/* loop through the pending io requests and find our next soonest timeout */
		list_for_each_entry(io, &hdev->io_head, list) {
			/* skip the timeout calculation if it's an isochronous request, or if
			 * the IO is not in progress (to avoid processing aborted requests) */
			if( (io->req->type == USB_TYPE_ISOCHRONOUS) ||
					(io->status != USBI_IO_INPROGRESS) ) {
				continue;
			}
			
			if (   io->tvo.tv_sec
					&& (!tvo.tv_sec || usbi_timeval_compare(&io->tvo, &tvo))) {
				/* new soonest timeout */
				memcpy(&tvo, &io->tvo, sizeof(tvo));
			}
		}
		pthread_mutex_unlock(&hdev->lock);

		/* save the next soonest timeout */
		memcpy(&tvNext, &tvo, sizeof(tvo));

		/* calculate the timeout for select() based on what we found above */
		if (!tvo.tv_sec) {
			/* Default to an hour from now */
			tvo.tv_sec = tvc.tv_sec + (60 * 60);
			tvo.tv_usec = tvc.tv_usec;
		} else if (usbi_timeval_compare(&tvo, &tvc) < 0) {
			/* Don't give a negative timeout */
			memcpy(&tvo, &tvc, sizeof(tvo));
		}

		/* Make tvo absolute time now */
		tvo.tv_sec -= tvc.tv_sec;
		if (tvo.tv_usec < tvc.tv_usec) {
			tvo.tv_sec--;
			tvo.tv_usec += (1000000 - tvc.tv_usec);
		} else {
			tvo.tv_usec -= tvc.tv_usec;
		}

		/* determine if we have file descriptors reading for reading/writing */
		ret = select(maxfd + 1, &readfds, &writefds, NULL, &tvo);
		if (ret < 0) {
			usbi_debug(hdev->lib_hdl, 1, "select() call failed: %s", strerror(errno));
			continue;
		}

		/* Get the current time of day, for timeout processing */
		gettimeofday(&tvc, NULL);

		pthread_mutex_lock(&hdev->lock);

		/* if there is data to be read on the event pipe read it and discard */
		if (FD_ISSET(hdev->priv->event_pipe[0], &readfds)) {
			read(hdev->priv->event_pipe[0], buf, 1);
			if(hdev->state == USBI_DEVICE_CLOSING) {
				/* device is closing, exit this thread */
				pthread_mutex_unlock(&hdev->lock);
				return NULL;
			}
		}

		/* If there is data to be read on the frontend's even pipe, read it.
		 * Even though the Linux backend doesn't use the frontend's timeout
		 * thread, the frontend will write to the even pipe everytime a request
		 * is submitted, so if we don't read the data out eventually we'll no
		 * longer be able to submit io requests (because the file buffer will
		 * fill up. */
		if (FD_ISSET(hdev->event_pipe[0], &readfds)) {
			read(hdev->event_pipe[0], buf, 1);
			if(hdev->state == USBI_DEVICE_CLOSING) {
				/* device is closing, exit this thread */
				pthread_mutex_unlock(&hdev->lock);
				return NULL;
			}
		}

		/* now that we've waited for select, determine what action to take */
		/* Have any io requests completed? */
		if (FD_ISSET(hdev->priv->fd, &writefds)) {
			io_complete(hdev);
		}

		/* Check for requests that may have timed out: This is handled by the
		 * separate timeout thread now */
		if (usbi_timeval_compare(&tvNext, &tvc) <= 0) {
			io_timeout(hdev, &tvc);
		}

		pthread_mutex_unlock(&hdev->lock);
	}

	return (NULL);
}



/*
 * create_new_device
 *
 *  Allocate memory for the new device and fill in the require information.
 */
int32_t create_new_device(struct usbi_device **dev, struct usbi_bus *ibus,
                          uint16_t devnum, uint32_t max_children)
{
	struct usbi_device *idev;

	idev = malloc(sizeof(*idev));
	if (!idev) {
		return (OPENUSB_NO_RESOURCES);
	}  
	memset(idev, 0, sizeof(*idev));

	idev->priv = calloc(sizeof(struct usbi_dev_private), 1);
	if (!idev->priv) {
		free(idev);
		return (OPENUSB_NO_RESOURCES);
	}

	idev->devnum = devnum;
	snprintf(idev->sys_path, sizeof(idev->sys_path) - 1, "%s/%03d",
					 ibus->sys_path, idev->devnum);
	usbi_debug(NULL, 4, "usbfs path: %s", idev->sys_path);

	idev->nports = max_children;
	if (max_children) {
		idev->children = malloc(idev->nports * sizeof(idev->children[0]));
		if (!idev->children) {
			free(idev);
			return (OPENUSB_NO_RESOURCES);
		}

		memset(idev->children, 0, idev->nports * sizeof(idev->children[0]));
	}

	*dev = idev;
	ibus->priv->dev_by_num[devnum] = idev;

	return (OPENUSB_SUCCESS);
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
		return (0);

	while ((entry = readdir(dir)) != NULL) {
		/* Skip anything starting with a . */
		if (entry->d_name[0] == '.')
			continue;

		/* We assume if we find any files that it must be the right place */
		found = 1;
		break;
	}

	closedir(dir);

	return (found);
}



/*
 * check_usb_path
 *
 *  Write data to the event pipe to wakeup the io thread
 */
int32_t wakeup_io_thread(struct usbi_dev_handle *hdev)
{
	uint8_t buf[1] = {1};

	buf[0] = 0;
	if (write(hdev->priv->event_pipe[1], buf, 1) < 1) {
		usbi_debug(hdev->lib_hdl, 1, "unable to write to event pipe: %s",
							 strerror(errno));
		return translate_errno(errno);
	}

	return OPENUSB_SUCCESS;
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
	uint32_t              count;
	usb_device_desc_t     device;
	int32_t               sts = OPENUSB_SUCCESS;
	int32_t               i, fd, ret;

	/* Validate... */
	if ((!idev) || (!buflen)) {
		return (OPENUSB_BADARG);
	}

	/* Right now we're only setup to do device and config descriptors */
	if ((type != USB_DESC_TYPE_DEVICE) && (type != USB_DESC_TYPE_CONFIG)) {
		usbi_debug(NULL, 1, "unsupported descriptor type");
		return (OPENUSB_BADARG);
	} 

	/* Open the device */
	fd = device_open(idev);
	if (fd < 0) {
		usbi_debug(NULL, 1, "couldn't open %s: %s", idev->sys_path,
							 strerror(errno));
		return (OPENUSB_UNKNOWN_DEVICE);
	}

	/* The way USBFS works we always have to read the data in order, so start by
	 * reading the device descriptor, no matter what descriptor we were asked for
	 */
	devdescr = malloc(USBI_DEVICE_DESC_SIZE);
	if (!devdescr) {
		usbi_debug(NULL, 1,
							 "unable to allocate memory for cached device descriptor");
		sts = OPENUSB_NO_RESOURCES;
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
	openusb_parse_data("bbwbbbbwwwbbbb", devdescr, devdescrlen, &device,
										 USBI_DEVICE_DESC_SIZE, &count);

	/* now we'll allocated memory for all of our config descriptors */
	configs_raw = malloc(device.bNumConfigurations * sizeof(configs_raw[0]));
	if (!configs_raw) {
		usbi_debug(NULL, 1, "unable to allocate memory for cached descriptors");
		sts = OPENUSB_NO_RESOURCES;
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
				usbi_debug(NULL, 1, "config descriptor too short (expected %d, got %d)",
									 8, ret);
			} 
			sts = translate_errno(errno);
			goto done;
		}

		openusb_parse_data("bbw", buf, 8, &cfg_desc, sizeof(cfg_desc), &count);
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
				usbi_debug(NULL, 1, "config descriptor too short (expected %d, got %d)",
									 cfgr->len, ret);
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
				sts = OPENUSB_NO_RESOURCES;
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
												 char *name, uint32_t namelen)
{
	struct usbk_getdriver getdrv;
	int ret;

	getdrv.interface = interface;
	ret = ioctl(hdev->priv->fd, IOCTL_USB_GETDRIVER, &getdrv);
	if (ret) {
		usbi_debug(hdev->lib_hdl, 1,
							 "could not get bound driver: %s",
							 strerror(errno));
		return (translate_errno(errno));
	}

	strncpy(name, getdrv.driver, namelen - 1);
	name[namelen - 1] = 0;

	return OPENUSB_SUCCESS;
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

	return (OPENUSB_SUCCESS);
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

	return (OPENUSB_SUCCESS);
}




#if 1 /* hal */
/*
 * linux_refresh_devices
 *
 *  Make a new search of the devices on the bus and refresh the device list.
 *  The device nodes that have been detached from the system would be removed 
 *  from the list.
 */
int32_t linux_refresh_devices(struct usbi_bus *ibus)
{
	int 								i;
	int 								num_devices;
	char 								**device_names;
	struct usbi_device	*idev, *tidev;
	DBusError						error;
	DBusConnection			*conn;
	LibHalContext				*hal_ctx;

	/* Validate... */
	if (!ibus) {
		return (OPENUSB_BADARG);
	}

	/* Lock the bus */
	pthread_mutex_lock(&ibus->lock);

	/* Initialize the error struct... */
	dbus_error_init (&error);

	/* Create & Initialize the HAL context */
	if ((hal_ctx = libhal_ctx_new()) == NULL) {
		usbi_debug(NULL, 1, "error: libhal_ctx_new");
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		usbi_debug(NULL, 1, "error: dbus_bus_get: %s: %s", error.name,
							 error.message);
		dbus_error_free(&error);
		libhal_ctx_free (hal_ctx);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	/* Set the DBUS connection */
	if (!libhal_ctx_set_dbus_connection (hal_ctx, conn)) {
		usbi_debug(NULL, 1, "error: libhal_ctx_set_dbus_connection: %s: %s\n",
							 error.name, error.message);
		libhal_ctx_free (hal_ctx);
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	/* Initialize the HAL context */
	if (!libhal_ctx_init (hal_ctx, &error)) {
		if (dbus_error_is_set(&error)) {
			usbi_debug(NULL, 1, "error: libhal_ctx_init: %s: %s\n",
								 error.name, error.message);
			dbus_error_free(&error);
		}
		usbi_debug(NULL, 1, "Could not initialise connection to hald.");
		usbi_debug(NULL, 1, "Normally this means the HAL daemon (hald) is");
		usbi_debug(NULL, 1, "not running or not ready.");
		libhal_ctx_free (hal_ctx);
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	/* Get an array of all the devices on the system */
	device_names = libhal_get_all_devices (hal_ctx, &num_devices, &error);
	if (device_names == NULL) {
		dbus_error_free(&error);
		usbi_debug(NULL, 1, "Couldn't obtain list of devices\n");
		libhal_ctx_free (hal_ctx);
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (OPENUSB_SYS_FUNC_FAILURE);
	}

	/* Loops through the devices that were found and look for usb devices to
	 * add to our list of known devices */
	for (i = 0;i < num_devices;i++) {
		process_new_device(hal_ctx, device_names[i], ibus);
	}

	/* free the devices list */
	libhal_free_string_array(device_names);
	
	/* make sure every device we currently have in the list was found,
	 * if not, remove it. */
	list_for_each_entry_safe(idev, tidev, &ibus->devices.head, bus_list) {
		if (!idev->found) {
			/* Device disappeared, remove it */
			usbi_debug(NULL, 2, "device %d removed", idev->devnum);
			usbi_remove_device(idev);
		}

		/* Setup the parent relationship */
		if (idev->priv->pdevnum) {
			idev->parent = ibus->priv->dev_by_num[idev->priv->pdevnum];
		} else {
			ibus->root = idev;
		}
	}

	/* unlock */
	pthread_mutex_unlock(&ibus->lock);
	
	/* Free the HAL context (don't shut it down or we won't get events) */
	libhal_ctx_free (hal_ctx);

	/* Close the DBUS connection */
	dbus_connection_close (conn);
	dbus_connection_unref (conn);

	return (OPENUSB_SUCCESS);
}



/*
 * find_device_by_udi
 *
 *	This function will find the device in our list of known devices with
 *	the specified HAL unique identifier (udi).
 */
struct usbi_device *find_device_by_udi(const char *udi)
{
	struct usbi_device	*idev = NULL;
	struct usbi_list		*pusbi_devices = usbi_get_devices_list();
	
	usbi_debug(NULL, 4, "searching device: %s", udi);
	
	pthread_mutex_lock(&usbi_devices.lock);
	list_for_each_entry(idev, &((*pusbi_devices).head), dev_list) {
		if (!idev->priv->udi) {
			continue;
		}

		if (strcmp(udi, idev->priv->udi) == 0) {
			pthread_mutex_unlock(&usbi_devices.lock);
			return (idev);
		}
	}
	pthread_mutex_unlock(&usbi_devices.lock);

	return (NULL);
}


void process_new_device(LibHalContext *hal_ctx, const char *udi,
												struct usbi_bus *ibus)
{
	char								*parent;
	char								*bus;
	DBusError						error;
	struct usbi_device	*idev;
	int 								busnum = 0, pdevnum = 0, devnum = 0, max_children = 0;

	/* Initialize the error structure */
	dbus_error_init(&error);

	/* Get the bus, so we know what type of device this is */
	bus = libhal_device_get_property_string(hal_ctx, udi, "info.bus", &error);
	if (dbus_error_is_set(&error)) {
		dbus_error_free(&error);
		return;
	}

	/* if this is not a usb device, we're not interested */
	if (strcmp(bus, "usb_device") != 0) {
		libhal_free_string(bus);
		return;
	}

	usbi_debug(NULL, 4, "processing new device: %s", udi);

	/* We need to know the bus number, device number, parent device number,
	 * parent port and the maximum number of children this device can have
	 * in order to add it to our list */
	busnum = libhal_device_get_property_int(hal_ctx, udi, 
																					"usb_device.bus_number", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 4, "get device bus number error: %s", error.message);
		dbus_error_free(&error);
		libhal_free_string(bus);
		return;
	}

	/* if ibus is not NULL then we don't want to process this device unless
   * the specified bus number is the same */
	if (ibus != NULL) {
		/* Check the bus number to see if this device is on the specified bus */
		if (busnum != ibus->busnum) {
			libhal_free_string(bus);
			return;
		}
	} else {

		/* find the ibus that this device is on */
		ibus = usbi_find_bus_by_num(busnum);
		if (!ibus) {
			usbi_debug(NULL, 4, "Unable to find bus by number: %d", busnum);
			return;
		}
	}

	/* Get the device number */
	devnum = libhal_device_get_property_int(hal_ctx, udi,
																					"usb_device.linux.device_number",
																					&error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 4, "get device number error: %s", error.message);
		dbus_error_free(&error);
		libhal_free_string(bus);
		return;
	}

	/* Get the parent and it's device number */
	parent = libhal_device_get_property_string(hal_ctx, udi, 
																						 "info.parent", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 4, "Error getting parent device name: %s", error.message);
		dbus_error_free(&error);
		libhal_free_string(bus);
		return;
	}

	pdevnum = libhal_device_get_property_int(hal_ctx, parent,
			"usb_device.linux.device_number", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 4, "Error getting parent device number: %s",error.message);
		dbus_error_free(&error);
		/* this means that there probably isn't a parent device number, so
		 * make it zero, meaning this is the root device */
		pdevnum = 0;
	}
		
	/* Get the number of ports (aka max_children) */
	max_children = libhal_device_get_property_int(hal_ctx, udi,
			"usb_device.num_ports", &error);
	if (dbus_error_is_set(&error)) {
		usbi_debug(NULL, 4, "Error getting the number of ports: %s", error.message);
		dbus_error_free(&error);
	}
		
	/* Validate what we have so far */
	if (devnum < 1 || devnum >= USB_MAX_DEVICES_PER_BUS ||
			max_children >= USB_MAX_DEVICES_PER_BUS ||
			pdevnum >= USB_MAX_DEVICES_PER_BUS) {
		usbi_debug(NULL, 1, "invalid device number or parent device");
		libhal_free_string(bus);
		return;
	}

	/* Make sure we don't have two root devices */
	if (!pdevnum && ibus->root && ibus->root->found) {
		usbi_debug(NULL, 1, "cannot have two root devices");
		libhal_free_string(bus);
		return;
	}

	/* Only add this device if it's new */
	/* If we don't have a device by this number yet, it must be new */
	idev = ibus->priv->dev_by_num[devnum];
	if (!idev) {
		int ret;

		ret = create_new_device(&idev, ibus, devnum, max_children);
		if (ret) {
			usbi_debug(NULL, 1, "ignoring new device because of errors");
			libhal_free_string(bus);
			return;
		}

		/* set the parent device number */
		idev->priv->pdevnum = pdevnum;
		
		/* copy the udi */
		idev->priv->udi = strdup(udi);
		
		/* add the device */
		usbi_add_device(ibus, idev);

		/* Setup the parent relationship, if this device is new */
		if (idev->priv->pdevnum) {
			idev->parent = ibus->priv->dev_by_num[idev->priv->pdevnum];
		} else {
			ibus->root = idev;
		}

	}

	/* Mark the device as found */
	idev->found = 1;

	/* free the property strings */
	libhal_free_string(bus);

	return;
}



/*
 * Invoked when a device is added to the Global Device List.
 *
 */
void device_added (LibHalContext *ctx, const char *udi)
{
	struct usbi_device *idev = NULL;
	struct usbi_handle *handle, *thdl;

	usbi_debug(NULL, 4, "Event: device_added, udi='%s'", udi);

	idev = find_device_by_udi(udi);
	if (idev) {
		/* old device re-inserted */
		usbi_debug(NULL, 4, "old device: %d", (int)idev->devid);
		pthread_mutex_lock(&usbi_handles.lock);
		list_for_each_entry_safe(handle, thdl, &usbi_handles.head, list) {
			/* every openusb instance should get notification
			 * of this event */
			usbi_add_event_callback(handle, idev->devid, USB_ATTACH);
		}
		pthread_mutex_unlock(&usbi_handles.lock);
	} else {
		process_new_device(ctx, udi, NULL);
	}
}



/*
 * Invoked when a device is removed from the Global Device List.
 *
 */
void device_removed(LibHalContext *ctx, const char *udi)
{
	struct usbi_device	*idev = NULL;
	
	usbi_debug(NULL, 4, "Event: device_removed, udi='%s'", udi);
	
	idev = find_device_by_udi(udi);
	if (idev) {
		/* Clear the dev_by_num field */
		idev->bus->priv->dev_by_num[idev->devid] = NULL;
		
		/* removed the device */
		usbi_remove_device(idev);
	}

	/* If the device wasn't found we don't do anything */
	return;
}



/*
 * hotplug event monitoring thread
 */
void *hal_hotplug_event_thread(void *unused)
{
	LibHalContext		*hal_ctx;
	DBusConnection	*conn;
	DBusError				error;
	GMainContext		*gmaincontext;
	
	usbi_debug(NULL, 4, "starting hotplug thread...");
	
	/* Create the gmaincontext and the event loop */
	gmaincontext = g_main_context_new();
	event_loop = g_main_loop_new (gmaincontext, FALSE);

	/* Initialize the error structure */
	dbus_error_init(&error);
	
	conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		usbi_debug(NULL, 1, "error: dbus_bus_get: %s: %s", error.name,
							 error.message);
		dbus_error_free(&error);
		return (NULL);
	}

	/* Create & Initialize the HAL gmaincontext */
	if ((hal_ctx = libhal_ctx_new()) == NULL) {
		usbi_debug(NULL, 1, "error: libhal_ctx_new");
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (NULL);
	}
	
	/* setup the glib/DBUS connection */
	dbus_connection_setup_with_g_main(conn, gmaincontext);

	/* Set the DBUS connection */
	if (!libhal_ctx_set_dbus_connection (hal_ctx, conn)) {
		usbi_debug(NULL, 1, "error: libhal_ctx_set_dbus_connection: %s: %s\n",
							 error.name, error.message);
		libhal_ctx_free (hal_ctx);
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (NULL);
	}

	/* Initialize the HAL gmaincontext */
	if (!libhal_ctx_init (hal_ctx, &error)) {
		if (dbus_error_is_set(&error)) {
			usbi_debug(NULL, 1, "error: libhal_ctx_init: %s: %s\n",
								 error.name, error.message);
			dbus_error_free(&error);
		}

		usbi_debug(NULL, 1, "Could not initialise connection to hald.");
		usbi_debug(NULL, 1, "Normally this means the HAL daemon (hald) is");
		usbi_debug(NULL, 1, "not running or not ready.");
		libhal_ctx_free (hal_ctx);
		dbus_connection_close (conn);
		dbus_connection_unref (conn);
		return (NULL);
	}

	libhal_ctx_set_device_added (hal_ctx, device_added);
	libhal_ctx_set_device_removed (hal_ctx, device_removed);

	/* run the main loop */
	if (event_loop != NULL) {
		usbi_debug(NULL, 4, "hotplug thread running...");
		g_main_loop_run (event_loop);
		usbi_debug(NULL, 4, "hotplug thread exiting...");
	}

	if (libhal_ctx_shutdown (hal_ctx, &error) == FALSE) {
		dbus_error_free(&error);
	}
	libhal_ctx_free (hal_ctx);

	dbus_connection_close (conn);
	dbus_connection_unref (conn);
	
	g_main_context_unref(gmaincontext);
	g_main_context_release(gmaincontext);
	
	return (NULL);
}


#endif








struct usbi_backend_ops backend_ops = {
	.backend_version						= 1,
	.io_pattern									= PATTERN_ASYNC,
	.init												= linux_init,
	.fini												= linux_fini,
	.find_buses									= linux_find_buses,
	.refresh_devices						= linux_refresh_devices,
	.free_device								= linux_free_device,
	.dev = {
		.open											= linux_open,
		.close										= linux_close,
		.set_configuration				= linux_set_configuration,
		.get_configuration				= linux_get_configuration,
		.claim_interface					= linux_claim_interface,
		.release_interface				= linux_release_interface,
		.get_altsetting						= linux_get_altsetting,
		.set_altsetting						= linux_set_altsetting,
		.reset										= linux_reset,
		.get_driver_np						= linux_get_driver,
		.attach_kernel_driver_np	= linux_attach_kernel_driver,
		.detach_kernel_driver_np	= linux_detach_kernel_driver,
		.ctrl_xfer_aio						= linux_submit_ctrl,
		.intr_xfer_aio						= linux_submit_bulk_intr,
		.bulk_xfer_aio						= linux_submit_bulk_intr,
		.isoc_xfer_aio						= linux_submit_isoc,
		.ctrl_xfer_wait						= NULL,
		.intr_xfer_wait						= NULL,
		.bulk_xfer_wait						= NULL,
		.isoc_xfer_wait						= NULL,
		.io_cancel								= linux_io_cancel,
		.get_raw_desc							= linux_get_raw_desc,
	},
};
