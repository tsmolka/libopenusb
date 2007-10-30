/*
 * Main API entry point
 *
 * Copyright (c) 2007 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * Copyright 2000-2005 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>	/* getenv */
#include <stdio.h>	/* stderr */
#include <stdarg.h>	/* vsnprintf */
#include <string.h>	/* memset,strncpy */
#include <sys/time.h>	/* gettimeofday */
#include <dirent.h>	/* readdir */
#include <dlfcn.h>	/* dlopen */
#include <pthread.h>
#include <errno.h>

#include "usbi.h"

#define	USB_MAX_DEBUG_LEVEL	5

/*
 * env variables:
 *	LIBUSB_DRIVER_PATH - file path of backends
 *	USB_DEBUG - debug level of a libusb instance
 */

static int32_t libusb_global_debug_level = 0;

static libusb_handle_t cur_handle = 1; /* protected by usbi_lock */
static libusb_dev_handle_t cur_dev_handle = 1; /* protected by usbi_lock */

static int usbi_inited = 0;
static pthread_mutex_t usbi_lock = PTHREAD_MUTEX_INITIALIZER;

struct list_head backends = { .prev = &backends, .next = &backends };

/*
 * event callbacks list, all libusb instances share the same list
 * this list has its own lock
 */
static struct usbi_list event_callbacks;

/* the background thread processing events */
static pthread_t event_callback_thread;
static pthread_cond_t event_callback_cond;

void _usbi_debug(struct usbi_handle *hdl, uint32_t level, const char *func,
	uint32_t line, char *fmt, ...)
{
	char str[512];
	va_list ap;


	if ((!hdl) && (level > libusb_global_debug_level))
		return;

	if (hdl) {
		if (level > hdl->debug_level) {
			return;
		}

		pthread_mutex_lock(&hdl->lock);
	}

	va_start(ap, fmt);

	if (hdl && hdl->debug_cb) {
		snprintf(str, sizeof (str), "libusb: [%s:%d] %s", func,
			line, fmt);
		hdl->debug_cb(hdl->handle, str,ap);
	} else {
		vsnprintf(str, sizeof (str), fmt, ap);
		fprintf(stderr, "libusb: [%s:%d] %s\n", func, line, str);
	}

	va_end(ap);

	if(hdl) {
		pthread_mutex_unlock(&hdl->lock);
	}
}

struct eventcallback {
	struct list_head list;
	libusb_devid_t devid;
	libusb_event_t type;
	struct usbi_handle *handle;
};

int callback_queue_full = 0;

/* set callback for every libusb instance.*/
void usbi_add_event_callback(struct usbi_handle *hdl, libusb_devid_t devid, 
	libusb_event_t type)
{
	struct eventcallback *cb;

	usbi_debug(hdl, 4, "hdl=%p,handle=%llu,devid=%llu,type=%d", hdl,
		hdl->handle, devid, type);

	/* FIXME: Return/log error if malloc fails? */
	cb = calloc(sizeof(struct eventcallback), 1);
	if (!cb) {
		usbi_debug(hdl, 1, "allocate memory fail");
		return;
	}

	cb->devid = devid;
	cb->type = type;
	cb->handle = hdl;
	list_init(&cb->list);

	pthread_mutex_lock(&event_callbacks.lock);
	
	list_add(&cb->list, &event_callbacks.head);

	pthread_cond_signal(&event_callback_cond);
	
	if (callback_queue_full == 0)
		callback_queue_full = 1;

	pthread_mutex_unlock(&event_callbacks.lock);
}

static void *process_event_callbacks(void *unused)
{
	while (1) {
		struct eventcallback *cb = NULL;
		struct list_head *listh;

		pthread_mutex_lock(&event_callbacks.lock);

		while(callback_queue_full == 0) {
			pthread_cond_wait(&event_callback_cond, 
					&event_callbacks.lock);
		}

		/*
		 * Don't use list_for_each_entry(). It's not easy to free cb,
		 * because cb will be used at every iteration of "for" loop.
		 */
		listh = event_callbacks.head.next;
		while(listh != &event_callbacks.head) {
			libusb_devid_t devid;
			libusb_event_t type;
			struct usbi_handle *hdl;
			libusb_event_callback_t func;
			void *arg;

			cb = list_entry(&(listh->prev), struct eventcallback,
				list);

			list_del(&cb->list);

			devid = cb->devid;
			type = cb->type;
			hdl = cb->handle;

			func = hdl->event_cbs[type].func;
			arg = hdl->event_cbs[type].arg;

			/* Risk: if func blocks, no new event can be added.
			 * 	Release lock before call event callback
			 */
			pthread_mutex_unlock(&event_callbacks.lock);

			if (func) {
				usbi_debug(hdl, 4, "callback called");
				func(hdl->handle,devid, type, arg);
			} else {
				usbi_debug(hdl, 4, "No callback");
			}

			pthread_mutex_lock(&event_callbacks.lock);

			listh = listh->next;

			/* don't reference any element of cb after this */
			free(cb);
		}

		/* the list should be empty here */
		callback_queue_full = 0;

		pthread_mutex_unlock(&event_callbacks.lock);
	}
}

static int load_backend(const char *filepath)
{
	struct usbi_backend *backend;
	struct usbi_backend_ops *ops;
	int version, io_pattern;
	void *handle;

	handle = dlopen(filepath, RTLD_LAZY);
	if (!handle) {
		fprintf(stderr, "dlerror: %s\n", dlerror());
		goto err;
	}

	ops = dlsym(handle, "backend_ops");
	if (!ops) {
		fprintf(stderr, "no backend ops, skipping\n");
		goto err;
	}

	io_pattern = ops->io_pattern;

	if (io_pattern > PATTERN_BOTH || io_pattern < PATTERN_ASYNC) {
		fprintf(stderr, "backend io pattern is %d,"
			"not a valid pattern\n", io_pattern);
		goto err;
	}

	version = ops->backend_version;
	if (version != 1) {
		fprintf(stderr, "backend is API version %d, we need version 1\n",
			version);
		goto err;
	}

	backend = malloc(sizeof(*backend));
	if (!backend) {
		fprintf(stderr, "couldn't allocate memory for backend\n");
		goto err;
	}

	strncpy(backend->filepath, filepath, PATH_MAX);
	backend->handle = handle;
	backend->ops = ops;

	list_add(&backend->list, &backends);

	usbi_debug(NULL,4, "load backend");
	
	return 0;

err:
	dlclose(handle);
	return 1;
}

/* load all backends in the dirpath
 * 	Return -1  - fail to open the dirpath
 *	        0  - load successfully
 *	       >1  - more than one module fail to load
 */
static int load_backends(const char *dirpath)
{
	char filepath[PATH_MAX];
	struct dirent *entry;
	DIR *dir;
	int err_load=0;

	usbi_debug(NULL, 4, "open dirpath %s", dirpath);
	dir = opendir(dirpath);
	if (!dir) {
		usbi_debug(NULL, 1, "fail open %s",dirpath);
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		struct usbi_backend *backend;
		int found = 0;
		char *p;

		usbi_debug(NULL, 4, "backend entry %s", entry->d_name);
		
		/* string end */
		p = strchr(entry->d_name, 0);
		if (p - entry->d_name < 3)
			continue;

		/* not start with lib */
		if (strncmp(entry->d_name, "lib", 3) == 0)
			continue;

		/* not end with .so */
		if (strcmp(p - 3, ".so") != 0)
			continue;

		/*backend should have this format: !(lib)*.so */
		snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, 
			entry->d_name);
		usbi_debug(NULL, 4, "find backend path %s", filepath);

		list_for_each_entry(backend, &backends, list) {
		/* safe */
			if (strcmp(filepath, backend->filepath) == 0) {
				found = 1;
				break;
			}
		}

		if (!found) {
			if(load_backend(filepath) != 0) {
				err_load++;
				fprintf(stderr,"fail to load %s\n", filepath);
			}
		}
	}
	closedir(dir);

	return err_load;
}

/* 
 * init libusb internal lists
 */
static int usbi_list_init(struct usbi_list *list)
{
	int ret=0;

	list_init(&list->head);

	if ((ret = pthread_mutex_init(&list->lock, NULL)) != 0)
		return LIBUSB_SYS_FUNC_FAILURE;

	return ret;
}

static void usbi_list_fini(struct usbi_list *list)
{
	/* XXX do we need to free the list entries? */
	pthread_mutex_destroy(&list->lock);
}

/*
 * common init function. Get called only once
 */
static int usbi_init_common(void)
{
	const char *driver_path;
	int ret;

	/* set global debug level to default level */
	if (getenv("USB_DEBUG"))
		libusb_global_debug_level = atoi(getenv("USB_DEBUG"));

	/* Initialize the lib handle list */
	if ((ret = usbi_list_init(&usbi_handles)) < 0) {
		usbi_debug(NULL, 1, "unable to init lib handle list "
			"(ret = %d)", ret);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	/* Initializa the bus list */
	/* all libusb instances share the same bus list */
	if ((ret = usbi_list_init(&usbi_buses)) < 0) {
		usbi_debug(NULL, 1, "unable to init bus list "
			"(ret = %d)", ret);
		usbi_list_fini(&usbi_handles);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	/* Initializa the device list 
	 * all libusb instances share the same devices list
	 */
	if ((ret = usbi_list_init(&usbi_devices)) < 0) {
		usbi_debug(NULL, 1, "unable to init device list "
			"(ret = %d)", ret);
		usbi_list_fini(&usbi_buses);
		usbi_list_fini(&usbi_handles);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	/* Initializa the device handle list 
	 * all libusb instances share the same open devices list
	 */
	if ((ret = usbi_list_init(&usbi_dev_handles)) < 0) {
		usbi_debug(NULL, 1, "unable to init device handle list "
			"(ret = %d)", ret);
		usbi_list_fini(&usbi_devices);
		usbi_list_fini(&usbi_buses);
		usbi_list_fini(&usbi_handles);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	/* Initialize the callback list and thread
	 * all libusb instances share the same event_callback list
	 * and one callback processing thread
	 */
	if ((ret = usbi_list_init(&event_callbacks)) < 0) {
		usbi_debug(NULL, 1, "unable to init callback list "
			"(ret = %d)", ret);
		usbi_list_fini(&usbi_dev_handles);
		usbi_list_fini(&usbi_devices);
		usbi_list_fini(&usbi_buses);
		usbi_list_fini(&usbi_handles);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	pthread_cond_init(&event_callback_cond, NULL);

	/* Start up thread for callbacks */
	ret = pthread_create(&event_callback_thread, NULL,
		process_event_callbacks, NULL);
	if (ret < 0) {
		usbi_debug(NULL, 1, "unable to create callback thread "
			"(ret = %d)", ret);
		pthread_cond_destroy(&event_callback_cond);
		usbi_list_fini(&event_callbacks);
		usbi_list_fini(&usbi_dev_handles);
		usbi_list_fini(&usbi_devices);
		usbi_list_fini(&usbi_buses);
		usbi_list_fini(&usbi_handles);

		return LIBUSB_SYS_FUNC_FAILURE;
	}

	/* Load backends. All libusb instances share the same backends */
	load_backends(DRIVER_PATH); /* may need to check error */

	driver_path = getenv("LIBUSB_DRIVER_PATH");

	if (driver_path) {
		usbi_debug(NULL, 4, "backend path: %s",driver_path);
		load_backends(driver_path);
	}	
	
	/*
	 * actually, the usbi_bus ops only points to one backend's ops
	 * why libusb creates a backend list ?
	 */
	if(list_empty(&backends)) {
		usbi_debug(NULL, 1, "load backends fail");
		return LIBUSB_PLATFORM_FAILURE;
	}

	usbi_debug(NULL, 4, "End");

	return LIBUSB_SUCCESS;
}


/* called upon last libusb instance fini */
static void usbi_fini_common()
{
	/* XXX need to free device, bus and backend list */

	/* shared thread and cond should be destroyed ? */

	pthread_cond_destroy(&event_callback_cond);
	usbi_list_fini(&event_callbacks);
	usbi_list_fini(&usbi_dev_handles);
	usbi_list_fini(&usbi_devices);
	usbi_list_fini(&usbi_buses);
	usbi_list_fini(&usbi_handles);

}

struct usbi_handle *usbi_find_handle(libusb_handle_t handle)
{ 
	struct usbi_handle *hdl;

	/* fail if libusb is not inited */
	pthread_mutex_lock(&usbi_lock);
	if (usbi_inited == 0) {
		pthread_mutex_unlock(&usbi_lock);
		return NULL;
	}
	pthread_mutex_unlock(&usbi_lock);

	pthread_mutex_lock(&usbi_handles.lock);
	list_for_each_entry(hdl, &usbi_handles.head, list) {
	/* safe */
		if (hdl->handle == handle) {
			pthread_mutex_unlock(&usbi_handles.lock);
			return hdl;
		}
	}
	pthread_mutex_unlock(&usbi_handles.lock);

	return NULL;
}

/* malloc and init usbi_handle */
struct usbi_handle *usbi_init_handle(void)
{
	struct usbi_handle *hdl;
	int ret;

	hdl = malloc(sizeof (struct usbi_handle));
	if (hdl == NULL) {
		usbi_debug(NULL, 1, "malloc handle failed (ret = %d)", errno);

		return NULL;
	}
	memset(hdl, 0, sizeof (struct usbi_handle));

	if ((ret = pthread_mutex_init(&hdl->lock, NULL)) != 0) {
		usbi_debug(NULL, 1, "init handle mutex failed (ret = %d)",
				ret);

		free(hdl);

		return NULL; 
	}

	/* set debug level to default level */
	if (getenv("USB_DEBUG"))
		hdl->debug_level = atoi(getenv("USB_DEBUG"));

	/* the mutex protects cur_handle */
	pthread_mutex_lock(&usbi_lock);
	hdl->handle = cur_handle++;
	pthread_mutex_unlock(&usbi_lock);
	
	pthread_mutex_lock(&usbi_handles.lock);
	list_add(&hdl->list, &usbi_handles.head);
	pthread_mutex_unlock(&usbi_handles.lock);

	list_init(&hdl->complete_list);
	pthread_mutex_init(&hdl->complete_lock,NULL);
	pthread_cond_init(&hdl->complete_cv,NULL);
	hdl->complete_count = 0;

	return hdl;
}

/* destroy a usbi_handle and free its resouces */
void usbi_destroy_handle(struct usbi_handle *hdl)
{
	usbi_debug(NULL, 4, "Begin");

	if(hdl == NULL) {
		usbi_debug(NULL, 1, "Destroy handle fail");
		return;
	}

	pthread_mutex_lock(&usbi_handles.lock);
	list_del(&hdl->list);
	pthread_mutex_unlock(&usbi_handles.lock);

	pthread_mutex_destroy(&hdl->lock); /* may fail */

	pthread_mutex_destroy(&hdl->complete_lock);
	pthread_cond_destroy(&hdl->complete_cv);

	free(hdl);
}

int32_t libusb_init(uint32_t flags, libusb_handle_t *handle)
{
	struct usbi_handle *hdl;
	int ret;
	struct usbi_backend *backend;
	int back_cnt = 0;
	int init_cnt = 0;
	
	if(!handle) {
		return LIBUSB_BADARG;
	}

	*handle = 0;

	/* init the common part only on the first call */
	pthread_mutex_lock(&usbi_lock);
	if (usbi_inited == 0) {
		if ((ret = usbi_init_common()) < 0) {
			usbi_debug(NULL, 1, "usbi_init_common failed "
				"(ret = %d)", ret);
			pthread_mutex_unlock(&usbi_lock);

			return ret;
		}
	}
	usbi_inited++;
	pthread_mutex_unlock(&usbi_lock);

	hdl = usbi_init_handle();
	if(hdl == NULL) {
		pthread_mutex_lock(&usbi_lock);
		usbi_inited--;
		if (usbi_inited == 0)
			usbi_fini_common();
		pthread_mutex_unlock(&usbi_lock);

		return LIBUSB_SYS_FUNC_FAILURE;
	}
	
	list_for_each_entry(backend,&backends,list) {
	/* safe */
		back_cnt++;
		/* call backend init func */
		if (backend->ops->init(hdl, flags)) {
			usbi_debug(NULL, 1, "backend init fail");
			init_cnt++;
		}
	}
	
	/* no backends init succeed */
	if (back_cnt == init_cnt) {
		pthread_mutex_lock(&usbi_lock);
		usbi_inited--;
		if (usbi_inited == 0)
			usbi_fini_common();
		pthread_mutex_unlock(&usbi_lock);
		
		free(hdl);
		return LIBUSB_PLATFORM_FAILURE;
	}

	/*set up device tree */
	usbi_rescan_devices();

	*handle = hdl->handle;
	usbi_debug(hdl, 4, "End");

	return LIBUSB_SUCCESS;
}

void libusb_fini(libusb_handle_t handle)
{
	struct usbi_handle *hdl;
	struct usbi_backend *backend, *tbackend;

	usbi_debug(NULL, 4, "Begin");

	hdl = usbi_find_handle(handle);
	if(hdl == NULL) {
		usbi_debug(NULL, 1, "lib handle null");
		return;
	}

	list_for_each_entry_safe(backend, tbackend, &backends, list) {
	/* safe */
		if(backend->ops->fini)
			backend->ops->fini(hdl);/* call each backend's fini */
	}

	usbi_destroy_handle(hdl);

	pthread_mutex_lock(&usbi_lock);
	usbi_inited--;
	if (usbi_inited == 0) {
		usbi_debug(NULL, 4, "Last lib handle");
		usbi_fini_common();

		/* last libusb instance, destroy lock */
		pthread_mutex_unlock(&usbi_lock);
		pthread_mutex_destroy(&usbi_lock);
		return;
	}
	pthread_mutex_unlock(&usbi_lock);

	usbi_debug(NULL, 4, "End");
}

void usbi_coldplug_complete(struct usbi_handle *hdl)
{
	if (!hdl) {
		return;
	}

	pthread_mutex_lock(&hdl->lock);
	hdl->coldplug_complete = 1;
	pthread_cond_signal(&hdl->coldplug_cv);
	pthread_mutex_unlock(&hdl->lock);
}

/* 
 * set event callbacks for every libusb instance
 *	callback = NULL, unset previous callback settings
 */
int32_t libusb_set_event_callback(libusb_handle_t handle,
	libusb_event_t type, libusb_event_callback_t callback, void *arg)
{
	struct usbi_handle *hdl;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if (type < 0 || type >= LIBUSB_EVENT_TYPE_COUNT )
		return LIBUSB_BADARG;

	pthread_mutex_lock(&hdl->lock);
	hdl->event_cbs[type].func = callback;
	hdl->event_cbs[type].arg = arg;
	pthread_mutex_unlock(&hdl->lock);
	
	/* FIXME: just call coldplug_complete to prevent
	 * libusb_coldplug_callbacks_done() blocking.
	 */
	usbi_coldplug_complete(hdl);

	return LIBUSB_SUCCESS;
}

void libusb_set_debug(libusb_handle_t handle, uint32_t level,
	uint32_t flags, libusb_debug_callback_t callback)
{
	struct usbi_handle *hdl;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return;

	pthread_mutex_lock(&hdl->lock);

	if (callback) {
		hdl->debug_cb = callback;
	}

	hdl->debug_level = level;
	hdl->debug_flags = flags; /* not used, just prevent a warning */

	pthread_mutex_unlock(&hdl->lock);

	if (level) {
		usbi_debug(hdl, 4, "setting debugging level to %d (%s)",
			level, level ? "on" : "off");
	}
}

/* FIXME: do we need timeout for isoc transfer */
/* 	type = 0, set default timeout value for all types */
int32_t libusb_set_default_timeout(libusb_handle_t handle,
	libusb_transfer_type_t type, uint32_t timeout)
{
	struct usbi_handle *hdl;

	usbi_debug(NULL, 4, "Default timeout for type(%d): %d", type, timeout);

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	if ((type < 0) || (type > USB_TYPE_ISOCHRONOUS))
		return LIBUSB_BADARG;

	pthread_mutex_lock(&hdl->lock);

	if (type == USB_TYPE_ALL) {
		int i;
		for (i = USB_TYPE_CONTROL; i <= USB_TYPE_ISOCHRONOUS; i++) {
			hdl->timeout[i] = timeout;
		}
	} else {
		hdl->timeout[type] = timeout;
	}

	pthread_mutex_unlock(&hdl->lock);

	return LIBUSB_SUCCESS;
}

/* find an opened device's usbi_dev_handle by its libusb_dev_handle */
struct usbi_dev_handle *usbi_find_dev_handle(libusb_dev_handle_t dev)
{ 
	struct usbi_dev_handle *hdev;

	/* fail if libusb is not inited */
	pthread_mutex_lock(&usbi_lock);
	if (usbi_inited == 0) {
		pthread_mutex_unlock(&usbi_lock);
		return NULL;
	}
	pthread_mutex_unlock(&usbi_lock);

	/* FIXME: We should probably index the device id in a rbtree or
	 * something 
	 */
	pthread_mutex_lock(&usbi_dev_handles.lock);
	list_for_each_entry(hdev, &usbi_dev_handles.head, list) {
	/* safe */
		if (hdev->handle == dev) {
			pthread_mutex_unlock(&usbi_dev_handles.lock);
			return hdev;
		}
	}
	pthread_mutex_unlock(&usbi_dev_handles.lock);
	
	return NULL;
}   

/* find a device's usbi_device struct by its devid */
struct usbi_device *usbi_find_device_by_id(libusb_devid_t devid)
{
	struct usbi_device *idev;

	/* fail if libusb is not inited */
	pthread_mutex_lock(&usbi_lock);
	if (usbi_inited == 0) {
		pthread_mutex_unlock(&usbi_lock);
		return NULL;
	}
	pthread_mutex_unlock(&usbi_lock);

	/* FIXME: We should probably index the device id in a rbtree
	 * or something
	 */
	pthread_mutex_lock(&usbi_devices.lock);
	list_for_each_entry(idev, &usbi_devices.head, dev_list) {
	/* safe */
		if (idev->devid == devid) {
			pthread_mutex_unlock(&usbi_devices.lock);
			return idev;
		}
	}
	pthread_mutex_unlock(&usbi_devices.lock);

	return NULL;
}

/*
 * allocate libusb_dev_handle structure and populate it.
 * no device nodes opened at this moment on Solaris.
 */
int32_t libusb_open_device(libusb_handle_t handle, libusb_devid_t devid,
	libusb_init_flag_t flags, libusb_dev_handle_t *dev)
{
	struct usbi_handle *hdl; /* lib internal handle */
	struct usbi_device *idev;
	struct usbi_dev_handle *hdev;
	int ret;
	int i;
	
	if(!dev) {
		return LIBUSB_BADARG;
	}

	*dev = 0;

	hdl = usbi_find_handle(handle);
	if (!hdl)
		return LIBUSB_INVALID_HANDLE;

	idev = usbi_find_device_by_id(devid);
	if (!idev)
		return LIBUSB_UNKNOWN_DEVICE;

	hdev = malloc(sizeof (*hdev));
	if (!hdev)
		return LIBUSB_NO_RESOURCES;

	/* protect cur_dev_handle */
	pthread_mutex_lock(&usbi_lock);
	hdev->handle = cur_dev_handle++;
	pthread_mutex_unlock(&usbi_lock);

	hdev->lib_hdl = hdl;
	hdev->idev = idev;
	hdev->flags = flags;

	if (pthread_mutex_init(&hdev->lock,NULL) != 0) {
		free(hdev);
		return LIBUSB_SYS_FUNC_FAILURE;
	}

	for(i=0;i<USBI_MAXINTERFACES;i++) {
		hdev->claimed_ifs[i].clm= -1;
		hdev->claimed_ifs[i].altsetting = -1;
	}
	
	list_init(&hdev->io_head);
	list_init(&hdev->m_head);
	
	/* backend open will use event_pipe. so create pipe first */
	if (pipe(hdev->event_pipe) < 0) {
		pthread_mutex_destroy(&hdev->lock);
		free(hdev);
		return LIBUSB_SYS_FUNC_FAILURE;
	}

	ret = idev->ops->open(hdev);
	if (ret < 0) {
		pthread_mutex_destroy(&hdev->lock);
		free(hdev);
		return ret;
	}

	pthread_mutex_lock(&usbi_dev_handles.lock);

	list_add(&hdev->list, &usbi_dev_handles.head);
	hdev->state = USBI_DEVICE_OPENED;

	pthread_mutex_unlock(&usbi_dev_handles.lock);

	/* do we need to add the handle to idev and make a ref count */

	*dev = hdev->handle;

	return LIBUSB_SUCCESS;
}

int32_t libusb_close_device(libusb_dev_handle_t dev)
{
	struct usbi_dev_handle *hdev;
	int ret;
	struct usbi_io *io, *tio;

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;
	
	/* FIXME: need to abort the outstanding io request first */
	pthread_mutex_lock(&hdev->lock);

	list_for_each_entry_safe(io, tio, &hdev->io_head, list) {
		pthread_mutex_unlock(&hdev->lock);
		usbi_free_io(io);
		pthread_mutex_lock(&hdev->lock);
	}
	pthread_mutex_unlock(&hdev->lock);

	ret = hdev->idev->ops->close(hdev);

	pthread_mutex_lock(&usbi_dev_handles.lock);
	list_del(&hdev->list);
	pthread_mutex_unlock(&usbi_dev_handles.lock);

	pthread_mutex_destroy(&hdev->lock);

	close(hdev->event_pipe[0]);
	close(hdev->event_pipe[1]);

	free(hdev);

	return ret;
}

int32_t libusb_get_devid(libusb_dev_handle_t dev, libusb_devid_t *devid)
{
	struct usbi_dev_handle *hdev;

	if (!devid) {
		return LIBUSB_BADARG;
	}

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	*devid = hdev->idev->devid;

	return LIBUSB_SUCCESS;
}

int32_t libusb_get_lib_handle(libusb_dev_handle_t dev,
	libusb_handle_t *lib_handle)
{
	struct usbi_dev_handle *hdev;
	
	if (!lib_handle) {
		return LIBUSB_BADARG;
	}

	hdev = usbi_find_dev_handle(dev);
	if (!hdev)
		return LIBUSB_UNKNOWN_DEVICE;

	*lib_handle = hdev->lib_hdl->handle;

	return LIBUSB_SUCCESS;
}


int libusb_abort(libusb_request_handle_t phdl)
{
	struct usbi_dev_handle *hdev;
	struct usbi_io *io,*tio;
	int ret = LIBUSB_PLATFORM_FAILURE; 
	char buf[1]={1};

	if(!phdl) {
		return LIBUSB_INVALID_HANDLE;
	}

	/* We're looking at all open devices (open devices are ones we have
	 * handles for) and search for io requests with the specified tag.
	 * When we find one we'll cancel it. We don't lock here because we
	 * leave it up to the backend to handle that appropriately.
	 */
	list_for_each_entry(hdev, &usbi_dev_handles.head, list) {
		pthread_mutex_lock(&hdev->lock);
		list_for_each_entry_safe(io, tio, &hdev->io_head, list) {
			if (io->req == phdl) {
			/* Is it possible for one request to put on multiple
			 * device's request list? No
			 */
				ret = hdev->idev->ops->io_cancel(io);
				if (ret != 0) {
					usbi_debug(hdev->lib_hdl, 1,
						"abort error");
				} else {
					/* wake up timeout thread */
					write(hdev->event_pipe[1], buf, 1);

					/*free io?*/
				}
				pthread_mutex_unlock(&hdev->lock);
				return ret;
			}
		}
		pthread_mutex_unlock(&hdev->lock);
	}

	return (LIBUSB_INVALID_HANDLE); /* can't find specified request */
}


/*
 * We used to determine endian at build time, but this was causing problems
 * with cross-compiling, so I decided to try this instead. It determines
 * endian at runtime, the first time the code is run. This will be add some
 * extra cycles, but it should be insignificant. A really good compiler
 * might even be able to optimize away the code to figure out the endianess.
 */

inline uint16_t libusb_le16_to_cpu(uint16_t data)
{
	uint16_t endian = 0x1234;

	/* This test should be optimized away by the compiler */
	if (*(uint8_t *)&endian == 0x12) {
		unsigned char *p = (unsigned char *)&data;

		return p[0] | (p[1] << 8);
	} else {
		return data;
	}
}

inline uint32_t libusb_le32_to_cpu(uint32_t data)
{
	uint32_t endian = 0x12345678;

	/* This test should be optimized away by the compiler */
	if (*(uint8_t *)&endian == 0x12) {
		unsigned char *p = (unsigned char *)&data;

		return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
	} else {
		return data;
	}
}


/* FIXME: Maybe move these kinds of things to a util.c?
 * return:
 *	-1 timea < timeb
 * 	1  timea > timeb
 *	0  timea = timeb
 */
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
	{ LIBUSB_SUCCESS,		"Call success" },
	{ LIBUSB_PLATFORM_FAILURE,	"Unspecified kernel/driver failure" },
	{ LIBUSB_NO_RESOURCES,		"No resources available" },
	{ LIBUSB_NO_BANDWIDTH,		"No bandwidth available" },
	{ LIBUSB_NOT_SUPPORTED,		"Not supported by HCD" },
	{ LIBUSB_HC_HARDWARE_ERROR,	"USB host controller error" },
	{ LIBUSB_INVALID_PERM,		"Privileged operation" },
	{ LIBUSB_BUSY,			"Busy condition" },
	{ LIBUSB_BADARG,		"Invalid parameter" },
	{ LIBUSB_NOACCESS,		"Access to device denied" },
	{ LIBUSB_PARSE_ERROR,		"Data could not be parsed" },
	{ LIBUSB_UNKNOWN_DEVICE,	"Device id is stale or invalid" },
	{ LIBUSB_INVALID_HANDLE,		"Handle is invalid" },
	{ LIBUSB_SYS_FUNC_FAILURE,	"Call other system function failed" },
	{ LIBUSB_NULL_LIST,		"Can not find bus or device" },
	{ LIBUSB_IO_STALL,		"Endpoint stalled" },
	{ LIBUSB_IO_CRC_ERROR,		"CRC error" },
	{ LIBUSB_IO_DEVICE_HUNG,	"Device hung" },
	{ LIBUSB_IO_REQ_TOO_BIG,	"Request too big" },
	{ LIBUSB_IO_BIT_STUFFING,	"Bit stuffing error" },
	{ LIBUSB_IO_UNEXPECTED_PID,	"Unexpected PID" },
	{ LIBUSB_IO_DATA_OVERRUN,	"Data overrun" },
	{ LIBUSB_IO_DATA_UNDERRUN,	"Data underrun" },
	{ LIBUSB_IO_BUFFER_OVERRUN,	"Buffer overrun" },
	{ LIBUSB_IO_BUFFER_UNDERRUN,	"Buffer underrun" },
	{ LIBUSB_IO_PID_CHECK_FAILURE,	"PID check failure" },
	{ LIBUSB_IO_DATA_TOGGLE_MISMATCH, "Data toggle mismatch" },
	{ LIBUSB_IO_TIMEOUT,		"I/O timeout" },
	{ LIBUSB_IO_CANCELED,		"I/O canceled" } 
};

const char *libusb_strerror(int32_t error)
{
	int i;

	for (i = 0; i < sizeof (errorstrs) / sizeof (errorstrs[0]); i++) {
		if (errorstrs[i].code == error)
		return errorstrs[i].msg;
	}

	return "Unknown error";
}

/*
 * Adapted from Linux backend implementation
 * Suppose every opened device will have a thread to process timeout
 * This thread is created by backend
 */
void *timeout_thread(void *arg)
{
	struct usbi_dev_handle *devh;
	struct usbi_io *io, *tio;

	devh = (struct usbi_dev_handle *)arg;

	/* 
	 * Loop forever checking to see if we have io requests that need to be
	 * processed and process them.
	 */ 
	while (1) {
		struct timeval tvc, tvo;  
		fd_set readfds, writefds;
		int ret, maxfd;

		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		/* Always check the event_pipe and the devices file */
		FD_SET(devh->event_pipe[0], &readfds);

		maxfd = devh->event_pipe[0];

		gettimeofday(&tvc, NULL);

		memset(&tvo, 0, sizeof(tvo));

		/* 
		 * find the next soonest timeout so select() knows how
		 * long to wait
		 */  
		pthread_mutex_lock(&devh->lock);


		list_for_each_entry(io, &devh->io_head, list) {
		/* safe */

			/* avoid possible process on aborted io request */
			if (io->status != USBI_IO_INPROGRESS) {

				pthread_mutex_unlock(&devh->lock);
				continue;
			}

			if (io->tvo.tv_sec &&
				(!tvo.tv_sec ||
				 usbi_timeval_compare(&io->tvo, &tvo))) {
				/* New soonest timeout */

				memcpy(&tvo, &io->tvo, sizeof(tvo));
			}
		}
		pthread_mutex_unlock(&devh->lock);

		/* calculate the timeout for select() based on 
		 * what we found above
		 */

		if (!tvo.tv_sec) {
			/* Default to an hour from now */

			tvo.tv_sec = tvc.tv_sec + (60 * 60);
			tvo.tv_usec = tvc.tv_usec;
		} else if (usbi_timeval_compare(&tvo, &tvc) < 0) {

			/* Don't give a negative timeout */
			memcpy(&tvo, &tvc, sizeof(tvo));
		}

		/* Make tvo relative time now */
		tvo.tv_sec -= tvc.tv_sec;
		if (tvo.tv_usec < tvc.tv_usec) {
			tvo.tv_sec--;
			tvo.tv_usec += (1000000 - tvc.tv_usec);
		} else
			tvo.tv_usec -= tvc.tv_usec;

		/* determine if we have file descriptors reading for
		 * reading/writing 
		 * an new io request add or timeout reaching will trigger
		 * further processing
		 */
		ret = select(maxfd + 1, &readfds, NULL, NULL, &tvo);
		if (ret < 0) {
			usbi_debug(devh->lib_hdl, 1,
				"select() call failed: %s", strerror(errno));
			continue;
		}

		gettimeofday(&tvc, NULL);

		if (FD_ISSET(devh->event_pipe[0], &readfds)) {
			char buf[16];
			read(devh->event_pipe[0], buf, sizeof(buf));

			if(devh->state == USBI_DEVICE_CLOSING) {
			/* device is closing, exit this thread */

				return NULL;
			}
		}

		pthread_testcancel();
		/* now we'll process any pending io requests & timeouts */
		pthread_mutex_lock(&devh->lock);

		list_for_each_entry_safe(io, tio, &devh->io_head, list) {

			if (usbi_timeval_compare(&io->tvo, &tvc) <= 0) {

				usbi_io_complete(io, LIBUSB_IO_TIMEOUT, 0);

			}
		}

		pthread_mutex_unlock(&devh->lock);
	}

	return NULL;
}