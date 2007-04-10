/*
 * Main API entry point
 *
 * Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>	/* getenv */
#include <stdio.h>	/* stderr */
#include <stdarg.h>	/* vsnprintf */
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>

#include "usbi.h"
#include "linux.h"

#define USB_DEFAULT_DEBUG_LEVEL		4
static int usb_debug = USB_DEFAULT_DEBUG_LEVEL;
static int usb_inited = 0;
static libusb_debug_callback_t  debug_callback = NULL;

static struct list_head usbi_handles = { .prev = &usbi_handles, .next = &usbi_handles };
static libusb_dev_handle_t cur_handle = 1;

struct list_head backends = { .prev = &backends, .next = &backends };

void _usbi_debug(int level, const char *func, int line, char *fmt, ...)
{
  char str[512];
  va_list ap;

  if (level > usb_debug)
    return;

  va_start(ap, fmt);
  if (debug_callback) {
    snprintf(str, sizeof(str), "libusb: [%s:%d] %s", func, line, fmt);
    debug_callback(str,ap);
  } else {
    vsnprintf(str, sizeof(str), fmt, ap);
    fprintf(stderr, "libusb: [%s:%d] %s\n", func, line, str);
  }
  va_end(ap);
}

void libusb_set_debug(int level, int flags, libusb_debug_callback_t callback)
{
  if (callback) {
    debug_callback = callback;
  }

  usb_debug = level;
  flags = flags; /* this isn't used yet, so we just prevent a warning here */

  if (usb_debug || level) {
    usbi_debug(1,"setting debuggin level to %d (%s)", level, level ? "on" : "off");
  }
}


struct callback {
  struct list_head list;

  libusb_device_id_t devid;
  libusb_event_type_t type;
};

static struct list_head callbacks;
static pthread_t callback_thread;
static pthread_mutex_t callback_lock;
static pthread_cond_t callback_cond;
static struct usbi_event_callback usbi_event_callbacks[2];

static void *process_callbacks(void *unused)
{
  pthread_mutex_lock(&callback_lock);
  while (1) {
    struct list_head *tmp;

    pthread_cond_wait(&callback_cond, &callback_lock);

    tmp = callbacks.next;
    while (tmp != &callbacks) {
      struct callback *cb;
      libusb_device_id_t devid;
      libusb_event_type_t type;
      libusb_event_callback_t func;
      void *arg;

      /* Dequeue callback */
      cb = list_entry(tmp, struct callback, list);

      devid = cb->devid;
      type = cb->type;
      func = usbi_event_callbacks[type].func;
      arg = usbi_event_callbacks[type].arg;

      list_del(&cb->list);
      free(cb);

      pthread_mutex_unlock(&callback_lock);

      /* Make callback */
      if (func)
        func(devid, type, arg);

      pthread_mutex_lock(&callback_lock);
      tmp = callbacks.next;
    }
  }
}

static int load_backend(const char *filepath)
{
  struct usbi_backend *backend;
  struct usbi_backend_ops *ops;
  int *version, *io_pattern;
  void *handle;

  handle = dlopen(filepath, RTLD_LAZY);
  if (!handle) {
    fprintf(stderr, "dlerror: %s\n", dlerror());
    goto err;
  }

  version = dlsym(handle, "backend_version");
  if (!version) {
    fprintf(stderr, "no backend version, skipping\n");
    goto err;
  }

  if (*version != 1) {
    fprintf(stderr, "backend is API version %d, we need version 1\n", *version);
    goto err;
  }

  ops = dlsym(handle, "backend_ops");
  if (!ops) {
    fprintf(stderr, "no backend ops, skipping\n");
    goto err;
  }
  
  io_pattern = dlsym(handle, "backend_io_pattern");
  if (!io_pattern) {
    fprintf(stderr, "no backend io pattern, skipping\n");
    goto err;
  }

  if (*io_pattern > 2 || *io_pattern < 1) {
    fprintf(stderr, "backend io pattern is %d, not a valid pattern\n",
        *io_pattern);
    goto err;
  }

  backend = malloc(sizeof(*backend));
  if (!backend) {
    fprintf(stderr, "couldn't allocate memory for backend\n");
    goto err;
  }

  backend->filepath = strdup(filepath);
  backend->handle = handle;
  backend->ops = ops;
  backend->io_pattern = *io_pattern;

  if (ops->init()) {
    fprintf(stderr, "couldn't initialize backend\n");
    free(backend);
    goto err;
  }

  list_add(&backend->list, &backends);

  usbi_debug(3, "load backend");
  return 0;

err:
  dlclose(handle);

  return 1;
}

static int load_backends(const char *dirpath)
{
  char filepath[PATH_MAX];
  struct dirent *entry;
  DIR *dir;

  usbi_debug(3, "open dirpath %s", dirpath); 
  dir = opendir(dirpath);
  if (!dir)
    return 1;

  while ((entry = readdir(dir)) != NULL) {
    struct usbi_backend *backend;
    int found = 0;
    char *p;

    p = strchr(entry->d_name, 0);
    if (p - entry->d_name < 3)
      continue;

    if (strncmp(entry->d_name, "lib", 3) == 0)
      continue;

    if (strcmp(p - 3, ".so") != 0)
      continue;

    snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);
    usbi_debug(3, "find backend path %s", filepath);

    list_for_each_entry(backend, &backends, list) {
      if (strcmp(filepath, backend->filepath) == 0) {
        found = 1;
        break;
      }
    }

    if (!found)
      load_backend(filepath);
  }
  closedir(dir);
  
  return 0;
}

int libusb_init(void)
{
  const char *driver_path;
  int ret;

  /* if libusb_init() is already called then do not reinit */
  if (usb_inited == 1)
    return LIBUSB_SUCCESS;

  usb_inited = 1;

  if (getenv("USB_DEBUG") && usb_debug == USB_DEFAULT_DEBUG_LEVEL)
    libusb_set_debug(atoi(getenv("USB_DEBUG")),0,NULL);

  // Initialize our event callbacks
  usbi_event_callbacks[USB_ATTACH].func = (libusb_event_callback_t)NULL;
  usbi_event_callbacks[USB_DETACH].func = (libusb_event_callback_t)NULL;

  // Initialize the callback list and thread
  list_init(&callbacks);
  pthread_mutex_init(&callback_lock, NULL);
  pthread_cond_init(&callback_cond, NULL);

  /* Start up thread for callbacks */
  ret = pthread_create(&callback_thread, NULL, process_callbacks, NULL);
  if (ret < 0) {
    usbi_debug(1, "unable to create polling thread (ret = %d)", ret);
    usb_inited = 0;

    return LIBUSB_FAILURE;
  }

  /* Load backends */
  load_backends(DRIVER_PATH);

  driver_path = getenv("LIBUSB_DRIVER_PATH");
  if (driver_path)
    load_backends(driver_path);

  usbi_rescan_devices();

  return LIBUSB_SUCCESS;
}

int libusb_set_event_callback(libusb_event_type_t type,
	libusb_event_callback_t func, void *arg)
{
  if (type < 0 || type > 1)
    return LIBUSB_BADARG;

  pthread_mutex_lock(&callback_lock);
  usbi_event_callbacks[type].func = func;
  usbi_event_callbacks[type].arg = arg;
  pthread_mutex_unlock(&callback_lock);

  return LIBUSB_SUCCESS;
}

void usbi_callback(libusb_device_id_t devid, libusb_event_type_t type)
{
  struct callback *cb;

  /* FIXME: Return/log error if malloc fails? */
  cb = malloc(sizeof(*cb));
  if (!cb)
    return;

  cb->devid = devid;
  cb->type = type;

  pthread_mutex_lock(&callback_lock);
  list_add(&cb->list, &callbacks);

  pthread_cond_signal(&callback_cond);
  pthread_mutex_unlock(&callback_lock);
}

struct usbi_dev_handle *usbi_find_dev_handle(libusb_dev_handle_t dev)
{ 
  struct usbi_dev_handle *hdev;

  /* FIXME: We should probably index the device id in a rbtree or something */
  list_for_each_entry(hdev, &usbi_handles, list) {
    if (hdev->handle == dev)
      return hdev;
  }
   
  return NULL;
}   

int libusb_open(libusb_device_id_t devid, libusb_dev_handle_t *handle)
{
  struct usbi_device *idev;
  struct usbi_dev_handle *hdev;
  int ret;

  idev = usbi_find_device_by_id(devid);
  if (!idev)
    return LIBUSB_UNKNOWN_DEVICE;

  hdev = malloc(sizeof(*hdev));
  if (!hdev)
    return LIBUSB_NO_RESOURCES;

  hdev->handle = cur_handle++;	/* FIXME: Locking */
  hdev->idev = idev;
  hdev->interface = hdev->altsetting = -1;

  ret = idev->ops->open(hdev);
  if (ret < 0) {
    free(hdev);
    *handle = 0;
    return ret;
  }

  list_add(&hdev->list, &usbi_handles);

  *handle = hdev->handle;

  return LIBUSB_SUCCESS;
}

int libusb_get_device_id(libusb_dev_handle_t dev, libusb_device_id_t *devid)
{
  struct usbi_dev_handle *hdev;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  *devid = hdev->idev->devid;

  return LIBUSB_SUCCESS;
}

int libusb_close(libusb_dev_handle_t dev)
{
  struct usbi_dev_handle *hdev;
  int ret;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return LIBUSB_UNKNOWN_DEVICE;

  ret = hdev->idev->ops->close(hdev);
  list_del(&hdev->list);
  free(hdev);

  return ret;
}


int libusb_abort(unsigned int tag)
{
  struct usbi_dev_handle *hdev;
  struct usbi_io *io, *tio;
  int ret = LIBUSB_FAILURE; 

  /* We're looking at all open devices (open devices are ones we have handles
   * for) and search for io requests with the specified tag. When we find one
   * we'll cancel it. We don't lock here because we leave it up to the backend
   * to handle that appropriately.
   */
  list_for_each_entry(hdev, &usbi_handles, list) {
    list_for_each_entry_safe(io, tio, &hdev->ios, list) {
      if (io->tag == tag) {
        ret = hdev->idev->ops->io_cancel(io);
      }
    }
  }

  return ret;
}


/*
 * We used to determine endian at build time, but this was causing problems
 * with cross-compiling, so I decided to try this instead. It determines
 * endian at runtime, the first time the code is run. This will be add some
 * extra cycles, but it should be insignificant. A really good compiler
 * might even be able to optimize away the code to figure out the endianess.
 */

uint16_t libusb_le16_to_cpu(uint16_t data)
{
  uint16_t endian = 0x1234;

  /* This test should be optimized away by the compiler */
  if (*(uint8_t *)&endian == 0x12) {
    unsigned char *p = (unsigned char *)&data;

    return p[0] | (p[1] << 8);
  } else
    return data;
}

uint32_t libusb_le32_to_cpu(uint32_t data)
{
  uint32_t endian = 0x12345678;

  /* This test should be optimized away by the compiler */
  if (*(uint8_t *)&endian == 0x12) {
    unsigned char *p = (unsigned char *)&data;

    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  } else
    return data;
}

/*
 * Some helper code for sync I/O (Control, Interrupt and Bulk)
 */
struct simple_io {
  pthread_mutex_t lock;
  pthread_cond_t  complete;   /* signaled when the io is complete */
  int completed;              /* this provides an alternate signal */

  int status;
  size_t transferred_bytes;
};

static void simple_io_setup(struct simple_io *io)
{
  pthread_mutex_init(&io->lock, NULL);
  pthread_cond_init(&io->complete, NULL);
  
  io->completed = 0; 
}

static int simple_io_wait(struct simple_io *io,
	size_t *transferred_bytes)
{
  int status;

  /* Race Condition: We do not want to wait on io->complete if it's already
   * been signaled. Use io->completed == 1 as the signal this has happened. 
   */
  pthread_mutex_lock(&io->lock);
  if (!io->completed) {
    pthread_cond_wait(&io->complete, &io->lock);
  }
  pthread_mutex_unlock(&io->lock);

  *transferred_bytes = io->transferred_bytes;
  status = io->status; 

  return status;
}

static void simple_io_complete(struct simple_io *io,
	int status, size_t transferred_bytes)
{
  /* Race Condition: We do not want to wait on io->complete if it's already
   * been signaled. Use io->completed == 1 as the signal this has happened. 
   */
  pthread_mutex_lock(&io->lock); 
  io->completed = 1;
  pthread_mutex_unlock(&io->lock); 
  
  io->status = status;
  io->transferred_bytes = transferred_bytes;
  pthread_cond_signal(&io->complete);
  
}

/*
 * Control
 */
static void ctrl_callback(struct libusb_ctrl_request *ctrl, void *arg,
	int status, size_t transferred_bytes)
{
  simple_io_complete(arg, status, transferred_bytes);
}

int libusb_ctrl(struct libusb_ctrl_request *ctrl, size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(ctrl->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    struct simple_io io;

    simple_io_setup(&io);

    ret = usbi_async_ctrl_submit(ctrl, ctrl_callback, &io);
    if (ret)
      return ret;

    return simple_io_wait(&io, transferred_bytes);
  } else if (io_pattern == PATTERN_SYNC) {
    ret = usbi_sync_ctrl_submit(ctrl, transferred_bytes);

    return ret;
  } else {

    return LIBUSB_FAILURE;
  }
}

/*
 * Interrupt
 */
static void intr_callback(struct libusb_intr_request *intr, void *arg,
	int status, size_t transferred_bytes)
{
  simple_io_complete(arg, status, transferred_bytes);
}

int libusb_intr(struct libusb_intr_request *intr, size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(intr->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    struct simple_io io;

    simple_io_setup(&io);

    ret = usbi_async_intr_submit(intr, intr_callback, &io);
    if (ret)
      return ret;

    return simple_io_wait(&io, transferred_bytes);
  } else if (io_pattern == PATTERN_SYNC) {
    ret = usbi_sync_intr_submit(intr, transferred_bytes);

    return ret;
  } else {

    return LIBUSB_FAILURE;
  }
}

/*
 * Bulk
 */
static void bulk_callback(struct libusb_bulk_request *bulk, void *arg,
	int status, size_t transferred_bytes)
{
  simple_io_complete(arg, status, transferred_bytes);
}

int libusb_bulk(struct libusb_bulk_request *bulk, size_t *transferred_bytes)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(bulk->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    struct simple_io io;

    simple_io_setup(&io);

    ret = usbi_async_bulk_submit(bulk, bulk_callback, &io);
    if (ret)
      return ret;

    return simple_io_wait(&io, transferred_bytes);
  } else if (io_pattern == PATTERN_SYNC) {
    ret = usbi_sync_bulk_submit(bulk, transferred_bytes);

    return ret;
  } else {

    return LIBUSB_FAILURE;
  }
}

/*
 * Some helper code for async I/O (Control, Interrupt and Bulk)
 */
struct async_io {
  enum usbi_io_type type;
  void *request;
  void *callback;
  void *arg;
};

static void *
io_submit(void *arg)
{
  struct async_io *io = (struct async_io *)arg;
  size_t transferred_bytes;
  int ret;

  switch(io->type) {
  case USBI_IO_CONTROL:
  {
    struct libusb_ctrl_request *ctrl;
    libusb_ctrl_callback_t callback;

    ctrl = (struct libusb_ctrl_request *)io->request;
    callback = (libusb_ctrl_callback_t)io->callback;
    ret = usbi_sync_ctrl_submit(ctrl, &transferred_bytes);
    if (callback)
      callback(ctrl, io->arg, ret, transferred_bytes);

    break;
  }
  case USBI_IO_INTERRUPT:
  {
    struct libusb_intr_request *intr;
    libusb_intr_callback_t callback;

    intr = (struct libusb_intr_request *)io->request;
    callback = (libusb_intr_callback_t)io->callback;
    ret = usbi_sync_intr_submit(intr, &transferred_bytes);
    if (callback)
      callback(intr, io->arg, ret, transferred_bytes);
    
    break;
  }
  case USBI_IO_BULK:
  {
    struct libusb_bulk_request *bulk;
    libusb_bulk_callback_t callback;

    bulk = (struct libusb_bulk_request *)io->request;
    callback = (libusb_bulk_callback_t)io->callback;
    ret = usbi_sync_bulk_submit(bulk, &transferred_bytes);
    if (callback)
      callback(bulk, io->arg, ret, transferred_bytes);

    break;
  }
  case USBI_IO_ISOCHRONOUS:
  {
    struct libusb_isoc_request *iso;
    libusb_isoc_callback_t callback;

    /* FIXME: only NULL callback for isoc OUT is supported now */
    iso = (struct libusb_isoc_request *)io->request;
    callback = (libusb_isoc_callback_t)io->callback;
    ret = usbi_sync_isoc_submit(iso, NULL);
    if (callback)
      callback(iso, io->arg, 0, NULL);

    break;
  }
  }

  free(io);

  return NULL;
}

int libusb_ctrl_submit(struct libusb_ctrl_request *ctrl,
	libusb_ctrl_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(ctrl->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    ret = usbi_async_ctrl_submit(ctrl, callback, arg);
    return ret;
  } else if (io_pattern == PATTERN_SYNC) {
    pthread_t thrid;
    struct async_io *io;

    io = malloc(sizeof (*io));
    if (!io)
      return LIBUSB_NO_RESOURCES;

    memset(io, 0, sizeof(*io));

    io->type = USBI_IO_CONTROL;
    io->request = (void *)ctrl;
    io->callback = (void *)callback;
    io->arg = arg;

    /* create sync thread */
    ret = pthread_create(&thrid, NULL, io_submit, (void *)io);
    if (ret < 0) {
      free(io);
      return LIBUSB_FAILURE;
    }

    return LIBUSB_SUCCESS;
  } else {

    return LIBUSB_FAILURE;
  }
}

int libusb_intr_submit(struct libusb_intr_request *intr,
	libusb_intr_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(intr->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    ret = usbi_async_intr_submit(intr, callback, arg);
    return ret;
  } else if (io_pattern == PATTERN_SYNC) {
    pthread_t thrid;
    struct async_io *io;

    io = malloc(sizeof (*io));
    if (!io)
      return LIBUSB_NO_RESOURCES;

    memset(io, 0, sizeof(*io));

    io->type = USBI_IO_INTERRUPT;
    io->request = (void *)intr;
    io->callback = (void *)callback;
    io->arg = arg;

    /* create sync thread */
    ret = pthread_create(&thrid, NULL, io_submit, (void *)io);
    if (ret < 0) {
      free(io);
      return LIBUSB_FAILURE;
    }

    return LIBUSB_SUCCESS;
  } else {

    return LIBUSB_FAILURE;
  }
}

int libusb_bulk_submit(struct libusb_bulk_request *bulk,
	libusb_bulk_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(bulk->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if (io_pattern == PATTERN_ASYNC) {
    ret = usbi_async_bulk_submit(bulk, callback, arg);
    return ret;
  } else if (io_pattern == PATTERN_SYNC) {
    pthread_t thrid;
    struct async_io *io;

    io = malloc(sizeof (*io));
    if (!io)
      return LIBUSB_NO_RESOURCES;

    memset(io, 0, sizeof(*io));

    io->type = USBI_IO_BULK;
    io->request = (void *)bulk;
    io->callback = (void *)callback;
    io->arg = arg;

    /* create sync thread */
    ret = pthread_create(&thrid, NULL, io_submit, (void *)io);
    if (ret < 0) {
      free(io);
      return LIBUSB_FAILURE;
    }

    return LIBUSB_SUCCESS;
  } else {

    return LIBUSB_FAILURE;
  }
}

int libusb_isoc_submit(struct libusb_isoc_request *iso,
	libusb_isoc_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  int io_pattern, ret;

  dev = usbi_find_dev_handle(iso->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io_pattern = dev->idev->bus->io_pattern;

  if ((io_pattern == PATTERN_SYNC) &&
    ((iso->endpoint & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT)) {
    pthread_t thrid;
    struct async_io *io;

    io = malloc(sizeof (*io));
    if (!io)
      return LIBUSB_NO_RESOURCES;

    memset(io, 0, sizeof(*io));

    io->type = USBI_IO_ISOCHRONOUS;
    io->request = (void *)iso;
    io->callback = (void *)callback;
    io->arg = arg;

    /* create sync thread */
    ret = pthread_create(&thrid, NULL, io_submit, (void *)io);
    if (ret < 0) {
      free(io);
      return LIBUSB_FAILURE;
    }

    return LIBUSB_SUCCESS;
  } else {
    ret = usbi_async_isoc_submit(iso, callback, arg);
    return ret;
  }
}

/* FIXME: Maybe move these kinds of things to a util.c? */
int usbi_timeval_compare(struct timeval *tva, struct timeval *tvb)
{
  if (tva->tv_sec < tvb->tv_sec)
    return -1;
  else if (tva->tv_sec > tvb->tv_sec)
    return 1;

  if (tva->tv_usec < tvb->tv_usec)
    return -1;
  else if (tva->tv_usec > tvb->tv_usec)
    return 1;

  return 0;
}

static struct errorstr {
  int code;
  char *msg;
} errorstrs[] = {
  { LIBUSB_SUCCESS,                 "Call success" },
  { LIBUSB_FAILURE,                 "Unspecified error" },
  { LIBUSB_NO_RESOURCES,            "No resources available" },
  { LIBUSB_NO_BANDWIDTH,            "No bandwidth available" },
  { LIBUSB_NOT_SUPPORTED,           "Not supported by HCD" },
  { LIBUSB_HC_HARDWARE_ERROR,       "USB host controller error" },
  { LIBUSB_INVALID_PERM,            "Privileged operation" },
  { LIBUSB_BUSY,                    "Busy condition" },
  { LIBUSB_BADARG,                  "Invalid parameter" },
  { LIBUSB_NOACCESS,                "Access to device denied" },
  { LIBUSB_PARSE_ERROR,             "Data could not be parsed" },
  { LIBUSB_UNKNOWN_DEVICE,          "Device id is stale or invalid" },
  { LIBUSB_IO_STALL,                "Endpoint stalled" },
  { LIBUSB_IO_CRC_ERROR,            "CRC error" },
  { LIBUSB_IO_DEVICE_HUNG,          "Device hung" },
  { LIBUSB_IO_REQ_TOO_BIG,          "Request too big" },
  { LIBUSB_IO_BIT_STUFFING,         "Bit stuffing error" },
  { LIBUSB_IO_UNEXPECTED_PID,       "Unexpected PID" },
  { LIBUSB_IO_DATA_OVERRUN,         "Data overrun" },
  { LIBUSB_IO_DATA_UNDERRUN,        "Data underrun" },
  { LIBUSB_IO_BUFFER_OVERRUN,       "Buffer overrun" },
  { LIBUSB_IO_BUFFER_UNDERRUN,      "Buffer underrun" },
  { LIBUSB_IO_PID_CHECK_FAILURE,    "PID check failure" },
  { LIBUSB_IO_DATA_TOGGLE_MISMATCH, "Data toggle mismatch" },
  { LIBUSB_IO_TIMEOUT,              "I/O timeout" },
  { LIBUSB_IO_CANCELED,             "I/O canceled" } 
};

const char *libusb_strerror(int err)
{
  int i;

  for (i = 0; i < sizeof(errorstrs) / sizeof(errorstrs[0]); i++) {
    if (errorstrs[i].code == err)
      return errorstrs[i].msg;
  }

  return "Unknown error";
}
