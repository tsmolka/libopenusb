/*
 * Internal handling of IO requests with devices
 *
 * Copyright (c) 2007-2008 Sun Microsystems, Inc. All rights reserved  
 * Use is subject to license terms.
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

/*static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;*/
static struct list_head completions = { .prev = &completions,
		.next = &completions };

/*
 * Helper functions
 */

/* allocate usbi_io, caller must ensure arguments valid */
struct usbi_io *usbi_alloc_io(struct usbi_dev_handle *dev,
		openusb_request_handle_t req, uint32_t timeout) 
{
	struct usbi_io *io;
	struct timeval tvc;
	char buf[2];

	io = malloc(sizeof(*io));
	if (!io)
		return NULL;

	memset(io, 0, sizeof(*io));

	pthread_mutex_init(&io->lock, NULL);
	pthread_cond_init(&io->cond, NULL);

	pthread_mutex_lock(&io->lock);
	list_init(&io->list);
	
	io->dev = dev;
	if (timeout == 0) {
	/* set it to a big value to avoid the timeout thread delete it */
		timeout = io->timeout = 0xFFFFFFFF;
	} else {
		io->timeout = timeout;
	}

	io->status = USBI_IO_INPROGRESS;
	io->req = req;

	/* Set the end time for the timeout */
	gettimeofday(&tvc, NULL);
	io->tvo.tv_sec = tvc.tv_sec + timeout / 1000;
	io->tvo.tv_usec = tvc.tv_usec + (timeout % 1000) * 1000;

	if (io->tvo.tv_usec > 1000000) {
		io->tvo.tv_usec -= 1000000;
		io->tvo.tv_sec++;
	}
	pthread_mutex_unlock(&io->lock);

	/*timeout thread will process this list */
	pthread_mutex_lock(&dev->lock);

	/*
	 * add all outstanding io requests (incld SYNC&ASYNC)to
	 * this device's io_head
	 */
	list_add(&io->list,&dev->io_head);

	write(dev->event_pipe[1],buf, 1); /* notify timeout thread */

	pthread_mutex_unlock(&dev->lock);

	return io;
}

void usbi_free_io(struct usbi_io *io)
{
	char buf[1]={1};

	if (!io) {
		return;
	}

	pthread_mutex_lock(&io->lock);
	pthread_mutex_lock(&io->dev->lock);
	/* remove it from its original list to prevent
	 * other threads further processing on it
	 */
	list_del(&io->list);
	pthread_mutex_unlock(&io->dev->lock);

	if (io->status == USBI_IO_INPROGRESS && io->flag == USBI_ASYNC) {
		usbi_debug(io->dev->lib_hdl, 4, "IO is in progress, cancel it");
		if (io->dev->idev->ops->io_cancel) 
			io->dev->idev->ops->io_cancel(io);
	}
	
	write(io->dev->event_pipe[1], buf, 1); /* wakeup timeout thread */

	if (io->priv) {
		free(io->priv);
	}

	pthread_mutex_unlock(&io->lock);

	/* Delete the condition variable and wakeup any threads waiting */
	while (pthread_cond_destroy(&io->cond) == EBUSY) {
		pthread_mutex_lock(&io->lock);
		pthread_cond_broadcast(&io->cond);
		pthread_mutex_unlock(&io->lock);
	}

	pthread_mutex_destroy(&io->lock);

	free(io);
}

/* Helper routine. To be called from the various ports */
void usbi_io_complete(struct usbi_io *io, int32_t status, size_t transferred_bytes)
{
	openusb_request_result_t *result = NULL;
	openusb_transfer_type_t type;
	struct usbi_dev_handle *hdev = io->dev;

	pthread_mutex_lock(&io->lock);
	io->status = USBI_IO_COMPLETED;
	pthread_mutex_unlock(&io->lock);
	list_del(&io->list);
	
	/* Add completion for later retrieval */
	if (io->flag == USBI_ASYNC) {
		/* for synchronous IO, not necessary to put it on this list */
		pthread_mutex_lock(&hdev->lib_hdl->complete_lock);
		list_add(&io->list, &hdev->lib_hdl->complete_list);
		hdev->lib_hdl->complete_count++;
		pthread_cond_signal(&hdev->lib_hdl->complete_cv);
		pthread_mutex_unlock(&hdev->lib_hdl->complete_lock);
	}

	pthread_mutex_lock(&io->lock);
	type = io->req->type;

	if (type == USB_TYPE_CONTROL) {

		result = &io->req->req.ctrl->result;

	} else if (type == USB_TYPE_INTERRUPT) {

		result = &io->req->req.intr->result;

	} else if (type == USB_TYPE_BULK) {

		result = &io->req->req.bulk->result;

	} else if (type == USB_TYPE_ISOCHRONOUS) {

		result = &io->req->req.isoc->isoc_results[0];

	}
	pthread_mutex_unlock(&io->lock);

	result->status = status;
	result->transferred_bytes = transferred_bytes;

	pthread_mutex_lock(&io->lock);
	pthread_cond_broadcast(&io->cond);
	pthread_mutex_unlock(&io->lock);

	/* run the user supplied callback */
	if(io->req->cb) {	io->req->cb(io->req);	}

	/* run the internal callback, if it exists */
	if(io->callback) { io->callback(io,status);	}
	
	/* remove usbi_free_io */
}

/*
 * call backend's ASYNC xfer functions to submit this io
 */
int usbi_async_submit(struct usbi_io *io)
{
	struct usbi_dev_handle *dev;
	int ret;
	openusb_transfer_type_t type;
	
	pthread_mutex_lock(&io->lock);
	type = io->req->type;
	io->flag = USBI_ASYNC;
	pthread_mutex_unlock(&io->lock);
  
	dev = usbi_find_dev_handle(io->req->dev);
	if (!dev)
		return OPENUSB_UNKNOWN_DEVICE;

	if (type == USB_TYPE_CONTROL) {

		ret = dev->idev->ops->ctrl_xfer_aio(dev, io);

	} else if (type == USB_TYPE_INTERRUPT) {

		ret = dev->idev->ops->intr_xfer_aio(dev, io);

	} else if (type == USB_TYPE_BULK) {

		ret = dev->idev->ops->bulk_xfer_aio(dev, io);

	} else if (type == USB_TYPE_ISOCHRONOUS) {

		ret = dev->idev->ops->isoc_xfer_aio(dev, io);

	} else {

		ret = -1;
	}

	if (ret < 0) {
		/* removed usbi_free_io(io); it will happen later */
		return ret;
	}

	return OPENUSB_SUCCESS;
}

/*
 * this interface is for those SYNC mode backends
 * Backend should fill in all fields in io which need to be returned
 * to application, like result->status, result->transferred_bytes etc.
 */
int usbi_sync_submit(struct usbi_io *io)
{
	openusb_transfer_type_t type;
	struct usbi_dev_handle *dev;
	int ret;

	dev = io->dev;
	type = io->req->type;
	io->flag = USBI_SYNC;
	switch (type) {
		case USB_TYPE_CONTROL:
			ret = dev->idev->ops->ctrl_xfer_wait(dev, io);
			break;
		case USB_TYPE_INTERRUPT:
			ret = dev->idev->ops->intr_xfer_wait(dev, io);
			break;
		case USB_TYPE_ISOCHRONOUS:
			ret = dev->idev->ops->isoc_xfer_wait(dev, io);
			break;
		case USB_TYPE_BULK:
			ret = dev->idev->ops->bulk_xfer_wait(dev, io);
			break;
		default:
			ret = OPENUSB_BADARG;
	}

	/* upon success, the return value on Solaris is >= 0 */
	if (ret < 0) {
		return ret;
	}

	return OPENUSB_SUCCESS;
}

#if 1
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

static int simple_io_wait(struct simple_io *io)
{
	int status;

	/* Race Condition: We do not want to wait on io->complete if it's
	 * already been signaled. Use io->completed == 1 as the signal
	 * this has happened. 
	 */
	pthread_mutex_lock(&io->lock);
	if (!io->completed) {
		pthread_cond_wait(&io->complete, &io->lock);
	}
	status = io->status;
	pthread_mutex_unlock(&io->lock);
	
	return (status);
}

static void simple_io_complete(struct simple_io *io, int status)
{
	/* Race Condition: We do not want to wait on io->complete if it's
	 * already been signaled. Use io->completed == 1 as the signal this
	 * has happened. 
	 */
	pthread_mutex_lock(&io->lock); 

	io->completed = 1;
	io->status = status;

	pthread_cond_signal(&io->complete);
	pthread_mutex_unlock(&io->lock); 
}

/* backend is responsible to provide status of this io */
static void async_callback(struct usbi_io *io, int32_t status)
{
	simple_io_complete(io->arg, status);
}

/*
 * internal synchronous xfer function:
 *
 * If backend supports SYNC xfer, then call backend xfer functions directly.
 * Otherwise, we have to convert backend's ASYNC xfer to SYNC mode in these
 * functions.
 */
int usbi_io_sync(struct usbi_dev_handle *dev, openusb_request_handle_t req)
{
	int io_pattern, ret;

	io_pattern = dev->idev->bus->ops->io_pattern;

	if (io_pattern == PATTERN_ASYNC) {
		struct simple_io *io;
		struct usbi_io *iop;
		uint32_t timeout;
		
		timeout = usbi_get_xfer_timeout(req, dev);
		iop = usbi_alloc_io(dev, req, timeout);
		io = calloc(sizeof(*io), 1);
		if (!iop || !io) {
			return OPENUSB_NO_RESOURCES;
		}

		iop->callback = async_callback;
		iop->arg = io;

		simple_io_setup(io);
		ret = usbi_async_submit(iop);
		if (ret < 0) {
			usbi_free_io(iop);
			free(io);
			return ret;
		}

		ret = simple_io_wait(io);
		usbi_free_io(iop);
		free(io);
		return ret;

	} else if (io_pattern == PATTERN_SYNC) {
		struct usbi_io *io;
		uint32_t timeout;

		timeout = usbi_get_xfer_timeout(req, dev);

		io = usbi_alloc_io(dev, req, timeout);

		ret = usbi_sync_submit(io);
		usbi_free_io(io);

		return ret;
	} else {

		return OPENUSB_PLATFORM_FAILURE;
	}
}

/*
 * Some helper code for async I/O (Control, Interrupt and Bulk)
 */
struct async_io {
	openusb_transfer_type_t type;
	void *request;
	void *callback;
	void *arg;
};

static void*
io_submit(void *arg)
{
	struct usbi_io *iop = (struct usbi_io*)arg;
	int ret;
	struct usbi_dev_handle *dev;

	usbi_debug(iop->dev->lib_hdl, 4, "Begin: TID= %d",pthread_self());

	if (!iop) {
		return NULL;
	}

	/*remove this element from its original list */
	list_del(&iop->list);
	
	ret = usbi_sync_submit(iop);

	if(iop->callback){
		usbi_debug(iop->dev->lib_hdl, 4, "callback get called");
		iop->req->cb(iop->req);
		usbi_free_io(iop); /* should be removed ? */
	} else {
		/* somebody is waiting for this asnyc io,add it to
		 * complete list
		 */
		dev = iop->dev;

		usbi_debug(dev->lib_hdl, 4,
			"lib_hdl = %p,io = %p, cv=%p, lock=%p",
			dev->lib_hdl, iop, &dev->lib_hdl->complete_cv,
			&dev->lib_hdl->complete_lock);

		pthread_mutex_lock(&dev->lib_hdl->complete_lock);

		list_add(&iop->list,&dev->lib_hdl->complete_list);
		dev->lib_hdl->complete_count++;
		pthread_cond_signal(&(dev->lib_hdl->complete_cv));

		pthread_mutex_unlock(&dev->lib_hdl->complete_lock);
	}

	return NULL;
}

/*
 * internal function to submit ASYNC request.
 * If backend supports ASYNC mode, then call backend's xfer functions directly
 * Otherwise, we have to convert backend's xfer from SYNC mode to ASYNC mode.
 */
int usbi_io_async(struct usbi_io *iop)
{
	struct usbi_dev_handle *dev;
	int io_pattern;
	int ret = OPENUSB_PLATFORM_FAILURE;
	openusb_transfer_type_t type;

	pthread_mutex_lock(&iop->lock);
	dev = iop->dev;
	type = iop->req->type;
	pthread_mutex_unlock(&iop->lock);

	if (!dev)
		return OPENUSB_UNKNOWN_DEVICE;

	pthread_mutex_lock(&dev->idev->bus->lock);
	io_pattern = dev->idev->bus->ops->io_pattern;
	pthread_mutex_unlock(&dev->idev->bus->lock);
	
	if (type < USB_TYPE_CONTROL || type > USB_TYPE_ISOCHRONOUS) {
		return OPENUSB_BADARG;
	}

	if (io_pattern == PATTERN_ASYNC || io_pattern == PATTERN_BOTH) {
		ret = usbi_async_submit(iop);
		if (ret != 0) {
			usbi_debug(dev->lib_hdl, 1, "async_submit fail");
		}
		return ret;
	} else if (io_pattern == PATTERN_SYNC) {

		pthread_t thrid;

		/* create async thread */

		/* FIXME: every io is handled by a thread. It's hard to
		 * manage these threads. If device gets close, or application
		 * exit, we have no way to cancel these threads.
		 *
		 * I suggest to spawn one or a bunch of threads for every
		 * opened device.
		 * This thread will process async ios on this device's
		 * io_head list.
		 */
		ret = pthread_create(&thrid, NULL, io_submit, (void *)iop);

		if (ret < 0) {
			return OPENUSB_PLATFORM_FAILURE;
		}

		return OPENUSB_SUCCESS;
	} else {
		return OPENUSB_PLATFORM_FAILURE;
	}
}

openusb_request_handle_t usbi_alloc_request_handle(void)
{
	openusb_request_handle_t reqp;

	reqp = malloc(sizeof(struct openusb_request_handle));
	if (reqp == NULL) {
		return NULL;
	}

	memset(reqp, 0, sizeof(struct openusb_request_handle));
	return reqp;
}

#endif
