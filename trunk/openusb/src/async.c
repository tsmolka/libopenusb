/*
 * Handling of asynchronous IO with devices
 *
 * Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>	/* memset() */
#include <sys/time.h>	/* gettimeofday() */

#include "usbi.h"

static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head completions = { .prev = &completions, .next = &completions };

/*
 * Helper functions
 */
struct usbi_io *usbi_alloc_io(libusb_dev_handle_t dev, enum usbi_io_type type,
	unsigned char endpoint, unsigned int timeout)
{
  struct usbi_dev_handle *hdev;
  struct usbi_io *io;
  struct timeval tvc;

  hdev = usbi_find_dev_handle(dev);
  if (!hdev)
    return NULL;

  io = malloc(sizeof(*io));
  if (!io)
    return NULL;

  memset(io, 0, sizeof(*io));

  pthread_mutex_init(&io->lock, NULL);
  pthread_cond_init(&io->cond, NULL);

  list_init(&io->list);

  io->dev = hdev;
  io->type = type;
  io->endpoint = endpoint;
  io->timeout = timeout;

  /* Set the end time for the timeout */
  gettimeofday(&tvc, NULL);
  io->tvo.tv_sec = tvc.tv_sec + timeout / 1000;
  io->tvo.tv_usec = tvc.tv_usec + (timeout % 1000) * 1000;

  if (io->tvo.tv_usec > 1000000) {
    io->tvo.tv_usec -= 1000000;
    io->tvo.tv_sec++;
  }

  return io;
}

void usbi_free_io(struct usbi_io *io)
{
  pthread_mutex_lock(&io->lock);

  if (io->inprogress)
    io->dev->idev->ops->io_cancel(io);

  list_del(&io->list);

  if (io->tempbuf)
    free(io->tempbuf);

  if (io->type == USBI_IO_CONTROL)
    free(io->ctrl.setup);

  pthread_mutex_unlock(&io->lock);

  /* Delete the condition variable and wakeup any threads waiting */
  while (pthread_cond_destroy(&io->cond) == EBUSY) {
    pthread_mutex_lock(&io->lock);
    pthread_cond_broadcast(&io->cond);
    pthread_mutex_unlock(&io->lock);

    /* FIXME: Would be better to yield here, but there is no POSIX yield() */
    sleep(1);
  }
  pthread_mutex_destroy(&io->lock);

  free(io);
}

/* Helper routine. To be called from the various ports */
void usbi_io_complete(struct usbi_io *io, int status, size_t transferred_bytes)
{
  pthread_mutex_lock(&io->lock);
  io->inprogress = 0;
  pthread_mutex_unlock(&io->lock);

  /* Add completion for later retrieval */
  pthread_mutex_lock(&completion_lock);
  list_add(&io->list, &completions);
  pthread_mutex_unlock(&completion_lock);

  pthread_mutex_lock(&io->lock);
  pthread_cond_broadcast(&io->cond);
  pthread_mutex_unlock(&io->lock);

  switch (io->type) {
  case USBI_IO_CONTROL:
    io->ctrl.callback(io->ctrl.request, io->ctrl.arg, status, transferred_bytes);
    break;
  case USBI_IO_INTERRUPT:
    io->intr.callback(io->intr.request, io->intr.arg, status, transferred_bytes);
    break;
  case USBI_IO_BULK:
    io->bulk.callback(io->bulk.request, io->bulk.arg, status, transferred_bytes);
    break;
  case USBI_IO_ISOCHRONOUS:
    /* FIXME: Implement */
    break;
  }
}

/*
 * API implementation
 */
int libusb_io_cancel(libusb_dev_handle_t dev, unsigned char endpoint)
{
  /* FIXME: Implement */
  return LIBUSB_NOT_SUPPORTED;
}

int libusb_ctrl_submit(struct libusb_ctrl_request *ctrl,
	libusb_ctrl_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  unsigned char *setup;
  int ret;

  dev = usbi_find_dev_handle(ctrl->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  setup = malloc(USBI_CONTROL_SETUP_LEN);
  if (!setup)
    return LIBUSB_NO_RESOURCES;

  /* Fill in the SETUP packet */
  setup[0] = ctrl->setup.bRequestType;
  setup[1] = ctrl->setup.bRequest;
  *(uint16_t *)(setup + 2) = libusb_cpu_to_le16(ctrl->setup.wValue);
  *(uint16_t *)(setup + 4) = libusb_cpu_to_le16(ctrl->setup.wIndex);
  *(uint16_t *)(setup + 6) = libusb_cpu_to_le16(ctrl->buflen);

  io = usbi_alloc_io(ctrl->dev, USBI_IO_CONTROL, ctrl->endpoint, ctrl->timeout);
  if (!io) {
    free(setup);
    return LIBUSB_NO_RESOURCES;
  }

  io->ctrl.request = ctrl;
  io->ctrl.callback = callback;
  io->ctrl.arg = arg;

  io->ctrl.setup = setup;
  io->ctrl.buf = ctrl->buf;
  io->ctrl.buflen = ctrl->buflen;

  ret = dev->idev->ops->submit_ctrl(dev, io);
  if (ret < 0) {
    usbi_free_io(io);
    return ret;
  }

  return 0;
}

int libusb_intr_submit(struct libusb_intr_request *intr,
	libusb_intr_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  int ret;

  dev = usbi_find_dev_handle(intr->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io = usbi_alloc_io(intr->dev, USBI_IO_INTERRUPT, intr->endpoint, intr->timeout);
  if (!io)
    return LIBUSB_NO_RESOURCES;

  io->intr.request = intr;
  io->intr.callback = callback;
  io->intr.arg = arg;

  io->intr.buf = intr->buf;
  io->intr.buflen = intr->buflen;

  ret = dev->idev->ops->submit_intr(dev, io);
  if (ret < 0) {
    usbi_free_io(io);
    return ret;
  }

  return 0;
}

int libusb_bulk_submit(struct libusb_bulk_request *bulk,
	libusb_bulk_callback_t callback, void *arg)
{
  struct usbi_dev_handle *dev;
  struct usbi_io *io;
  int ret;

  dev = usbi_find_dev_handle(bulk->dev);
  if (!dev)
    return LIBUSB_UNKNOWN_DEVICE;

  io = usbi_alloc_io(bulk->dev, USBI_IO_BULK, bulk->endpoint, bulk->timeout);
  if (!io)
    return LIBUSB_NO_RESOURCES;

  io->bulk.request = bulk;
  io->bulk.callback = callback;
  io->bulk.arg = arg;

  io->bulk.buf = bulk->buf;
  io->bulk.buflen = bulk->buflen;

  ret = dev->idev->ops->submit_bulk(dev, io);
  if (ret < 0) {
    usbi_free_io(io);
    return ret;
  }

  return 0;
}

