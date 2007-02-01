#ifndef _USBI_H_
#define _USBI_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libusb.h"
#include "error.h"

/* Prevent namespace pollution */
#define list_init	__usb_list_init
#define list_add	__usb_list_add
#define list_del	__usb_list_del

#include "list.h"

#if defined(LINUX_API)
#include "linux.h"
#elif defined(BSD_API)
#include "bsd.h"
#elif defined(DARWIN_API)
#include "darwin.h"
#endif

struct usbi_event_callback {
  libusb_event_callback_t func;
  void *arg;
};

#define USB_MAX_DEVICES_PER_BUS		128	/* per the USB specs */

/* String descriptor */
struct usb_string_descriptor {
  uint8_t  bLength;
  uint8_t  bDescriptorType;
  uint16_t wData[1];
};

struct usbi_backend_ops;

/*
 * All of these structures are used for internal purposes and MUST not be
 * leaked out to the application so we can maintain forward compatibility.
 * We are free to change these structures between releases as we see fit.
 */
struct usbi_bus {
  struct list_head list;

  pthread_mutex_t lock;

  libusb_bus_id_t busid;
  unsigned int busnum;			/* Only needs to be unique */

  struct usbi_backend_ops *ops;

  struct list_head devices;
  struct usbi_device *root;

  USBI_BUS_PRIVATE
};

/* Sizes of various common descriptors */
#define USBI_DESC_HEADER_SIZE		2
#define USBI_DEVICE_DESC_SIZE		18
#define USBI_CONFIG_DESC_SIZE		9
#define USBI_INTERFACE_DESC_SIZE	9
#define USBI_ENDPOINT_DESC_SIZE		7
#define USBI_ENDPOINT_AUDIO_DESC_SIZE	9

struct usbi_endpoint {
  struct usb_endpoint_desc desc;
};

#define USBI_MAXENDPOINTS		32
struct usbi_altsetting {
  struct usb_interface_desc desc;

  size_t num_endpoints;
  struct usbi_endpoint *endpoints;
};

#define USBI_MAXALTSETTING		128	/* Hard limit */
struct usbi_interface {
  size_t num_altsettings;
  struct usbi_altsetting *altsettings;
};

#define USBI_MAXINTERFACES		32
struct usbi_config {
  struct usb_config_desc desc;

  size_t num_interfaces;
  struct usbi_interface *interfaces;
};

struct usbi_raw_desc {
  unsigned char *data;
  size_t len;
};

#define USBI_MAXCONFIG			8
struct usbi_descriptors {
  struct usbi_raw_desc device_raw;

  size_t num_configs;
  struct usbi_raw_desc *configs_raw;

  struct usb_device_desc device;
  struct usbi_config *configs;
};

struct usbi_device_ops;

struct usbi_device {
  struct list_head dev_list;
  struct list_head bus_list;

  libusb_device_id_t devid;
  unsigned int devnum;			/* Only needs to be unique */

  struct usbi_bus *bus;
  struct usbi_device_ops *ops;

  struct usbi_device *parent;		/* NULL for root hub */

  unsigned int num_ports;		/* Only for hubs */
  struct usbi_device **children;

  struct usbi_descriptors desc;

  /* These are the currently configured values */
  unsigned int cur_config;

  USBI_DEVICE_PRIVATE
};

/*
 * The usbi_dev_handle structure is defined per platform. It is used to
 * track an opened device. There are a couple of common members that are
 * defined with the USBI_DEV_HANDLE_COMMON macro. Everything else is up to
 * the platform. For instance, the Linux platform stuffs the fd into this
 * structure as well.
 */
struct usbi_dev_handle {
  /* FIXME: We should keep an array (or maybe list) of opened interfaces here */
  struct list_head list;

  libusb_dev_handle_t handle;
  struct usbi_device *idev;	/* device opened */

  unsigned int interface;	/* interface claimed */	
  unsigned int altsetting;	/* alternate setting */

  USBI_DEV_HANDLE_PRIVATE
};

struct usbi_match {
  struct list_head list;

  libusb_match_handle_t handle;

  unsigned int num_matches;
  unsigned int cur_match;

  unsigned int alloc_matches;
  libusb_device_id_t *matches;
};

#define USBI_CONTROL_SETUP_LEN	(1 + 1 + 2 + 2 + 2)

enum usbi_io_type {
  USBI_IO_CONTROL,
  USBI_IO_INTERRUPT,
  USBI_IO_BULK,
  USBI_IO_ISOCHRONOUS,
};

struct usbi_io {
  struct list_head list;

  pthread_mutex_t lock;		/* for all data in this structure (incl cond) */

  struct usbi_dev_handle *dev;

  enum usbi_io_type type;
  unsigned int endpoint;
  int inprogress;

  unsigned int timeout;
  struct timeval tvo;

  union {
    struct {
      struct libusb_ctrl_request *request;
      libusb_ctrl_callback_t callback;
      void *arg;

      void *setup;			/* SETUP packet for control messages */
      void *buf;			/* buffer of data */
      size_t buflen;			/* buffer size as submitted */
      size_t transferred_bytes;
    } ctrl;

    struct {
      struct libusb_intr_request *request;
      libusb_intr_callback_t callback;
      void *arg;

      void *buf;			/* buffer of data */
      size_t buflen;			/* buffer size as submitted */
      size_t transferred_bytes;
    } intr;

    struct {
      struct libusb_bulk_request *request;
      libusb_bulk_callback_t callback;
      void *arg;

      void *buf;			/* buffer of data */
      size_t buflen;			/* buffer size as submitted */
      size_t transferred_bytes;
    } bulk;

    struct {
      struct libusb_isoc_request *request;
      void *arg;

      libusb_isoc_callback_t callback;
      /* FIXME: Fill this in */
    } isoc;
  };

  void *tempbuf;		/* temporary use by backend */

  pthread_cond_t cond;		/* for waiting on completion */

  USBI_IO_HANDLE_PRIVATE
};

struct usbi_device_ops {
  int (*open)(struct usbi_dev_handle *dev);
  int (*close)(struct usbi_dev_handle *dev);
  int (*set_configuration)(struct usbi_device *idev, int cfg);
  int (*get_configuration)(struct usbi_device *idev, int *cfg);
  int (*claim_interface)(struct usbi_dev_handle *dev, int interface);
  int (*release_interface)(struct usbi_dev_handle *dev, int interface);
  int (*set_altinterface)(struct usbi_dev_handle *dev, int alt);
  int (*get_altinterface)(struct usbi_device *idev, int *alt);
  int (*reset)(struct usbi_dev_handle *dev);
  int (*get_driver_np)(struct usbi_dev_handle *dev, int interface,
		char *name, size_t namelen);
  int (*attach_kernel_driver_np)(struct usbi_dev_handle *dev, int interface);
  int (*detach_kernel_driver_np)(struct usbi_dev_handle *dev, int interface);
  int (*submit_ctrl)(struct usbi_dev_handle *hdev, struct usbi_io *io);
  int (*submit_intr)(struct usbi_dev_handle *hdev, struct usbi_io *io);
  int (*submit_bulk)(struct usbi_dev_handle *hdev, struct usbi_io *io);
  int (*submit_isoc)(struct usbi_dev_handle *hdev, struct usbi_io *io);
  int (*io_cancel)(struct usbi_io *io);
};

struct usbi_backend_ops {
  int (*init)(void);
  int (*find_busses)(struct list_head *busses);
  int (*refresh_devices)(struct usbi_bus *bus);
  struct usbi_device_ops dev;
};

struct usbi_backend {
  struct list_head list;
  void *handle;
  char *filepath;
  struct usbi_backend_ops *ops;
};

/* usb.c */
void _usbi_debug(int level, const char *func, int line, char *fmt, ...);
void usbi_callback(libusb_device_id_t devid, enum libusb_event_type type);
int usbi_timeval_compare(struct timeval *tva, struct timeval *tvb);
struct usbi_dev_handle *usbi_find_dev_handle(libusb_dev_handle_t dev);

#define usbi_debug(level, fmt...) _usbi_debug(level, __FUNCTION__, __LINE__, fmt)

/* async.c */
void usbi_io_complete(struct usbi_io *io, int status, size_t transferred_bytes);

/* descriptors.c */
void usb_fetch_descriptors(libusb_dev_handle_t dev);
void usbi_destroy_configuration(struct usbi_device *odev);
int usbi_parse_configuration(struct usbi_config *cfg, unsigned char *buf,
	size_t buflen);
int usbi_parse_device_descriptor(struct usbi_device *dev,
	unsigned char *buf, size_t buflen);

/* devices.c */
void usbi_free_bus(struct usbi_bus *bus);
void usbi_add_device(struct usbi_bus *ibus, struct usbi_device *idev);
void usbi_remove_device(struct usbi_device *idev);
void usbi_rescan_devices(void);
struct usbi_device *usbi_find_device_by_id(libusb_device_id_t devid);

#endif /* _USBI_H_ */

