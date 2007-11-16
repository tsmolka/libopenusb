#ifndef _WRAPPER_H_
#define _WRAPPER_H_

#include "libusb.h"
#include "list.h"

/* internal structure for backend */
struct usbi_backend {
	struct list_head	list;
	void			*handle; /* plugin object handle by dlopen() */
	char			filepath[PATH_MAX + 1]; /* plugin object path */
	struct usbi_backend_ops	*ops;
};

/* internal representation of USB bus, counterpart of libusb_busid_t */
struct usbi_bus {
	struct list_head	list;
	pthread_mutex_t		lock;
	libusb_busid_t		busid;
	char			sys_path[PATH_MAX + 1];
	struct usbi_backend_ops	*ops;
	struct list_head	devices; /* pointer to dev list on the bus */
	struct usbi_device	*root;	/* root device on the bus */
	uint32_t		ctrl_max_xfer_size;
	uint32_t		intr_max_xfer_size;
	uint32_t		bulk_max_xfer_size;
	uint32_t		isoc_max_xfer_size;
	struct usbi_bus_private	*priv;	/* backend specific data */
};

/* internal representation of USB device, counterpart of libusb_devid_t */
struct usbi_device {
	struct list_head	dev_list;
	struct list_head	bus_list;
	struct list_head	match_list; /* for search functions */
	libusb_devid_t		devid;
	uint8_t			bus_addr;
	struct usbi_bus		*bus;
	struct usbi_device	*parent;/* NULL for root hub */
	uint8_t			pport;	/* parent port */
	uint8_t			nports;	/* number of ports */
	char			sys_path[PATH_MAX + 1];
	char			bus_path[LIBUSB_BUS_PATH_MAX];
	struct usbi_device	**children;
	struct usbi_device_ops	*ops;
	uint8_t			cur_config;
	struct usbi_dev_private	*priv;	/* backend specific data */
};

struct usbi_event_callback {
	libusb_event_callback_t	func;
	void			*arg;
};

/* internal representation of libusb_handle_t */
struct usbi_handle {
	struct list_head	list;
	libusb_handle_t		handle;
	pthread_mutex_t		lock;
	uint32_t		debug_level;
	uint32_t		debug_flags;
	libusb_debug_callback_t	debug_cb;
	struct usbi_event_callback event_cbs[LIBUSB_EVENT_TYPE_COUNT];
	uint32_t		ctrl_timeout;	/* default timeout */
	uint32_t		intr_timeout;
	uint32_t		bulk_timeout;
	uint32_t		isoc_timeout;	/* do we need this for isoc? */
};

#define USBI_MAXINTERFACES	32

/* internal representation of libusb_dev_handle_t */
struct usbi_dev_handle {
	struct list_head	list;
	struct usbi_handle	*lib_hdl;
	libusb_dev_handle_t	handle;
	struct usbi_device	*idev;	/* device opened */
	libusb_init_flag_t	flags;	/* init flag */
	int			ifc[USBI_MAXINTERFACES]; /* =1 if claimed */
	int			alt[USBI_MAXINTERFACES];
	struct usbi_dev_hdl_private *priv; /* backend specific data */
};

/* internal representation of libusb I/O request */
struct usbi_io {
	struct list_head	list;
	pthread_mutex_t		lock;
	struct usbi_dev_handle	*dev;
	libusb_request_handle_t	req;
	int			inprogress; /* =1 if I/O in progress */
	struct timeval		tvo;
	pthread_cond_t		cond;	/* for waiting on completion */
	struct usbi_io_private	*priv;	/* backend specific data */
};

/*
 * The operation functions in the following two structures are backend
 * specific. Each backend needs to implement the functions as necessary.
 * The return values follow the definitions in libusb.h. For functions not
 * applicable to the backend, the backend can set them to NULL or return
 * LIBUSB_NOT_SUPPORTED.
 *
 * The function set is not final and can be changed as we see the need.
 */

struct usbi_device_ops {
	/* prepare device and make the default endpoint accessible to libusb */
	int32_t (*open)(struct usbi_dev_handle *dev);

	/* close device and return it to original state */
	int32_t (*close)(struct usbi_dev_handle *dev);

	/* configuration selection */
	int32_t (*set_configuration)(struct usbi_dev_handle *hdev,
		uint8_t cfg);
	int32_t (*get_configuration)(struct usbi_dev_handle *hdev,
		uint8_t *cfg);

	/* interface claiming and bandwidth reservation */
	int32_t (*claim_interface)(struct usbi_dev_handle *dev, uint8_t ifc);
	int32_t (*release_interface)(struct usbi_dev_handle *dev, uint8_t ifc);

	/* alternate setting selection */
	int32_t (*set_altsetting)(struct usbi_dev_handle *hdev,
		uint8_t ifc, uint8_t alt);
	int32_t (*get_altsetting)(struct usbi_device *idev,
		uint8_t ifc, uint8_t *alt);

	/* reset device by resetting port */
	int32_t (*reset)(struct usbi_dev_handle *dev);

	/* reset endpoint, for backward compatibility with libusb 0.1 */
	int32_t (*resetep)(struct usbi_dev_handle *dev, uint8_t ept);

	/* clear halted endpoint, for backward compatibility with libusb 0.1 */
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

	/* I/O abort function */
	int32_t (*io_cancel)(struct usbi_io *io);
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
	 * backend initialization, called in libusb_init()
	 *   flags - inherited from libusb_init(), TBD
	 */
	int32_t (*init)(uint32_t flags);

	/* backend specific data cleanup, called in libusb_fini() */
	void (*fini)(void);

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

	struct usbi_device_ops dev;
};

#endif /* _WRAPPER_H_ */

