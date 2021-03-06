/*
 * openusb internal data structures and function prototypes related
 * with backend
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */
#ifndef _USBI_H_
#define _USBI_H_

#include "openusb.h"

#include "list.h"
#include "descr.h"

#include <pthread.h>

struct usbi_list {
	struct list_head head;
	pthread_mutex_t lock;
};

/* internal structure for backend */
struct usbi_backend {
	struct list_head	list;
	void			*handle; /* plugin object handle by dlopen() */
	char			filepath[PATH_MAX + 1]; /* plugin object path */
	struct usbi_backend_ops	*ops;
};

/* internal representation of USB bus, counterpart of openusb_busid_t */
struct usbi_bus {
	struct list_head	list;
	pthread_mutex_t		lock;
	openusb_busid_t		busid;

	unsigned int busnum; /*a unique value, get from kernel on some OS */

	char			sys_path[PATH_MAX + 1]; /* file path */
	struct usbi_backend_ops	*ops;

	struct usbi_list devices; /* dev list on the bus */

	struct usbi_device	*root;	/* root device on the bus */

	uint32_t		max_xfer_size[USB_TYPE_LAST];
	struct usbi_bus_private	*priv;	/* backend specific data */
};

/* internal representation of USB device, counterpart of openusb_devid_t */
struct usbi_device {
	struct list_head	dev_list;
	struct list_head	bus_list;
	struct list_head	match_list; /* for search functions */

	openusb_devid_t		devid;

	unsigned int devnum; /* unique value, get from kernel on some OS */

	uint8_t			bus_addr;
	struct usbi_bus		*bus;
	struct usbi_device	*parent;/* NULL for root hub */
	uint8_t			pport;	/* parent port */
	uint8_t			nports;	/* number of ports */
	char			sys_path[PATH_MAX + 1];
	char			bus_path[OPENUSB_BUS_PATH_MAX];
	struct usbi_device	**children;
	struct usbi_device_ops	*ops;

	uint8_t			cur_config_value;/* bConfigurationValue */
	int			cur_config_index;/* the descriptor index of current configuration */

	struct usbi_dev_private	*priv;	/* backend specific data */
	
	int found; /* used by some backend for search */
	struct usbi_descriptors desc; /* temp */
};

struct usbi_event_callback {
	openusb_event_callback_t	func;
	void			*arg;
};

/* internal representation of openusb_handle_t */
struct usbi_handle {
	struct list_head	list;
	openusb_handle_t		handle;
	pthread_mutex_t		lock;
	uint32_t		debug_level;
	uint32_t		debug_flags;
	openusb_debug_callback_t	debug_cb;
	struct usbi_event_callback event_cbs[OPENUSB_EVENT_TYPE_COUNT];

	uint8_t		coldplug_complete;
	pthread_cond_t	coldplug_cv;

	/* all completed aio requests are queued here */
	struct list_head complete_list;

	pthread_mutex_t complete_lock;
	pthread_cond_t	complete_cv;
	int32_t complete_count;

	uint32_t	timeout[USB_TYPE_LAST];
};

#define USBI_IFC_UNCLAIMED 0
#define USBI_IFC_CLAIMED 1

struct interface_set {
	int clm; /* claimed? */
	int altsetting;
};

enum usbi_devstate{
	USBI_DEVICE_CLOSED,
	USBI_DEVICE_OPENED,
	USBI_DEVICE_CLOSING
};

#define USBI_MAXINTERFACES	32

/* internal representation of openusb_dev_handle_t */
struct usbi_dev_handle {
	struct list_head	list;

	/* keep track of this device's outstanding io requests */
	struct list_head io_head;

	struct list_head m_head; /* multi-xfer request list */

	struct usbi_handle	*lib_hdl;
	openusb_dev_handle_t	handle;
	struct usbi_device	*idev;	/* device opened */
	openusb_init_flag_t	flags;	/* init flag */

	/*claimed interfaces of this dev */
	struct interface_set claimed_ifs[USBI_MAXINTERFACES];

	pthread_mutex_t lock; /* protect all data field in this structure */

	int event_pipe[2]; /* event pipe */

	enum usbi_devstate state; /* device current state */

	struct usbi_dev_hdl_private *priv; /* backend specific data */
};

enum usbi_io_status {
	USBI_IO_INITIAL=0,
	USBI_IO_INPROGRESS,
	USBI_IO_CANCEL,
	USBI_IO_COMPLETED,
	USBI_IO_COMPLETED_FAIL,
	USBI_IO_TIMEOUT
};

#define USBI_ASYNC 1
#define USBI_SYNC 0

/* internal representation of openusb I/O request */
struct usbi_io {
	struct list_head	list;
	pthread_mutex_t		lock;
	struct usbi_dev_handle	*dev;
	openusb_request_handle_t	req;

	enum usbi_io_status status; /* status of this io request */

	uint32_t flag; /* SYNC/ASYNC */

  void (*callback)(struct usbi_io *io, int32_t status); /* internal callback */
	void *arg;	/* additional arguments the callback may use */

	struct timeval		tvo;
	uint32_t	timeout;

	pthread_cond_t		cond;	/* for waiting on completion */
	struct usbi_io_private	*priv;	/* backend specific data */
};

/*
 * The operation functions in the following two structures are backend
 * specific. Each backend needs to implement the functions as necessary.
 * The return values follow the definitions in openusb.h. For functions not
 * applicable to the backend, the backend can set them to NULL or return
 * OPENUSB_NOT_SUPPORTED.
 *
 * The function set is not final and can be changed as we see the need.
 */
struct usbi_device_ops {
	/* prepare device and make the default endpoint accessible to openusb */
	int32_t (*open)(struct usbi_dev_handle *dev);

	/* close device and return it to original state */
	int32_t (*close)(struct usbi_dev_handle *dev);

	/* configuration selection */
	int32_t (*set_configuration)(struct usbi_dev_handle *hdev,
		uint8_t cfg);
	int32_t (*get_configuration)(struct usbi_dev_handle *hdev,
		uint8_t *cfg);

	/* interface claiming and bandwidth reservation */
	int32_t (*claim_interface)(struct usbi_dev_handle *dev, uint8_t ifc,
		openusb_init_flag_t flag);
	int32_t (*release_interface)(struct usbi_dev_handle *dev, uint8_t ifc);

	/* alternate setting selection */
	int32_t (*set_altsetting)(struct usbi_dev_handle *hdev,
		uint8_t ifc, uint8_t alt);
	int32_t (*get_altsetting)(struct usbi_dev_handle *idev,
		uint8_t ifc, uint8_t *alt);

	/* reset device by resetting port */
	int32_t (*reset)(struct usbi_dev_handle *dev);

	/* reset endpoint, for backward compatibility with openusb 0.1 */
	int32_t (*resetep)(struct usbi_dev_handle *dev, uint8_t ept);

	/* clear halted endpoint, for backward compatibility with openusb 0.1 */
	int32_t (*clear_halt)(struct usbi_dev_handle *dev, uint8_t ept);

	/*
	 * synchronous I/O functions, might be NULL if not
	 * supported by backend
	 */
	int32_t (*ctrl_xfer_wait)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*intr_xfer_wait)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*bulk_xfer_wait)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*isoc_xfer_wait)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);

	/*
	 * asynchronous I/O functions, might be NULL if not
	 * supported by backend
	 */
	int32_t (*ctrl_xfer_aio)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*intr_xfer_aio)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*bulk_xfer_aio)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);
	int32_t (*isoc_xfer_aio)(struct usbi_dev_handle *hdev,
		struct usbi_io *io);

	/*
	 * get standard descriptor in its raw form
	 *   type - descriptor type
	 *   descidx - index for config/string desc., zero for others
	 *   langid - language ID for string desc., zero for others
	 *   buffer - backend allocated data buffer for raw desc.
	 *   buflen - backend returned length of raw desc.
	 */
	int32_t (*get_raw_desc)(struct usbi_device *idev, uint8_t type,
		uint8_t descidx, uint16_t langid,
		uint8_t **buffer, uint16_t *buflen);

	/* I/O abort function */
	int32_t (*io_cancel)(struct usbi_io *io);

	/* Non-portable functions */
	int32_t (*get_driver_np)(struct usbi_dev_handle *hdev, uint8_t interface,
													char *name, uint32_t namelen);
	int32_t (*attach_kernel_driver_np)(struct usbi_dev_handle *hdev,
																		 uint8_t interface);
	int32_t (*detach_kernel_driver_np)(struct usbi_dev_handle *hdev,
																		 uint8_t interface);
	
};

/* backend I/O pattern */
#define	PATTERN_ASYNC	1
#define	PATTERN_SYNC	2
#define	PATTERN_BOTH	4

/*
 * The backend must define a global variable named "backend_ops" of the
 * usbi_backend_ops structure type, so that the frontend can load the
 * symbol by dlsym().
 */
struct usbi_backend_ops {
	int backend_version;	/* need to match the frontend version */

	/*
	 * the pattern value indicates if the backend supports only
	 * asynchronous I/O functions or synchronous I/O functions
	 * or both
	 */
	int io_pattern;

	/*
	 * backend initialization, called in openusb_init()
	 *   flags - inherited from openusb_init(), TBD
	 */
	//int32_t (*init)(uint32_t flags);
	int32_t (*init)(struct usbi_handle *hdl, uint32_t flags);

	/* backend specific data cleanup, called in openusb_fini() */
	//void (*fini)(void);
	void (*fini)(struct usbi_handle *hdl);

	/*
	 * search USB buses under the control of the backend
	 * and return the bus list
	 */
	int32_t (*find_buses)(struct list_head *buses);

	/*
	 * make a new search of the devices on the bus and refresh
	 * the device list. the device nodes that have been detached
	 * from the system would be removed from the list
	 */
	int32_t (*refresh_devices)(struct usbi_bus *bus);

	/*
	 * cleanup backend specific data in the usbi_device structure.
	 * called when the device node is to be removed from the device list
	 */
	void (*free_device)(struct usbi_device *idev);


	struct usbi_device_ops dev;
};


/* defined in usb.c */
extern struct usbi_list usbi_handles; /* protected by usbi_handles.lock */
extern struct usbi_list usbi_dev_handles; /* protected by usbi_dev_handles.lock*/

extern struct usbi_list usbi_buses; /* protected by usbi_buses.lock */
extern struct usbi_list usbi_devices; /* protected by usbi_device.lock */


/*the following from old usbi.h */
#define USBI_CONTROL_SETUP_LEN (1 + 1 + 2 + 2 + 2)

#define USB_DEV_REQ_HOST_TO_DEV         0x00
#define USB_DEV_REQ_DEV_TO_HOST         0x80
#define USB_DEV_REQ_DIR_MASK            0x80

/* prototypes */

/* usb.c */
void usbi_callback(struct usbi_handle *hdl,openusb_devid_t devid,
	enum openusb_event type);

int usbi_timeval_compare(struct timeval *tva, struct timeval *tvb);
struct usbi_dev_handle *usbi_find_dev_handle(openusb_dev_handle_t dev);

void _usbi_debug(struct usbi_handle *hdl, uint32_t level, const char *func,
        uint32_t line, const char *fmt, ...);

void usbi_add_event_callback(struct usbi_handle *hdl, openusb_devid_t devid,
        openusb_event_t type);

#define usbi_debug(hdl, level, fmt...) \
       _usbi_debug(hdl, level, __FUNCTION__, __LINE__, fmt)

struct usbi_handle *usbi_find_handle(openusb_handle_t handle);

openusb_request_handle_t usbi_alloc_request_handle(void);
void *timeout_thread(void *arg);
int32_t usbi_get_driver_np(openusb_dev_handle_t dev, uint8_t interface,
			   char *name, uint32_t namelen);
int32_t usbi_attach_kernel_driver_np(openusb_dev_handle_t dev, uint8_t interface);
int32_t usbi_detach_kernel_driver_np(openusb_dev_handle_t dev, uint8_t interface);

/* io.c */
int usbi_io_sync(struct usbi_dev_handle *dev, openusb_request_handle_t req);
int usbi_io_async(struct usbi_io *iop);

void usbi_io_complete(struct usbi_io *io, int32_t status,
	size_t transferred_bytes);

struct usbi_io *usbi_alloc_io(struct usbi_dev_handle *dev,
	openusb_request_handle_t req, unsigned int timeout);
void usbi_free_io(struct usbi_io *io);

int usbi_async_submit(struct usbi_io *io);
int usbi_sync_submit(struct usbi_io *io);

/* descriptors.c */
int usbi_fetch_and_parse_descriptors(struct usbi_dev_handle *hdev);
void usbi_destroy_configuration(struct usbi_device *odev);
int usbi_parse_configuration(struct usbi_config *cfg, unsigned char *buf,
	size_t buflen);
int usbi_parse_device_descriptor(struct usbi_device *dev,
	unsigned char *buf, size_t buflen);

/* devices.c */
void usbi_free_bus(struct usbi_bus *bus);
void usbi_add_device(struct usbi_bus *ibus, struct usbi_device *idev);
void usbi_remove_device(struct usbi_device *idev);
void usbi_free_device(struct usbi_device *idev);
void usbi_rescan_devices(void);
struct usbi_device *usbi_find_device_by_id(openusb_devid_t devid);
struct usbi_bus *usbi_find_bus_by_num(unsigned int busnum);
int usbi_get_string(openusb_dev_handle_t dev, int index, int langid, char *buf,
    size_t buflen);
int usbi_get_string_simple(openusb_dev_handle_t dev, int index, char *buf,
    size_t buflen);
struct usbi_list *usbi_get_devices_list(void);
uint8_t usbi_get_cfg_value_by_index(struct usbi_dev_handle *hdev, int cfgndx);
int usbi_get_cfg_index_by_value(struct usbi_dev_handle *hdev, uint8_t cfgval);

/* api.c */
int32_t usbi_get_xfer_timeout(openusb_request_handle_t req, 
	struct usbi_dev_handle *dev);
int32_t usbi_control_xfer(struct usbi_dev_handle *devh,int requesttype,
	int request, int value, int index, char *bytes, int size, int timeout);

#endif /* _WRAPPER_H_ */
