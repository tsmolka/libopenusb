#include <stdlib.h>	/* getenv, etc */
#include <unistd.h>	/* read/write */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <libdevinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <config_admin.h>
#include <pthread.h>
#include "usbi.h"
#include <sys/usb/clients/ugen/usb_ugen.h>

static int busnum = 0;
static di_node_t root_node;
static di_devlink_handle_t devlink_hdl;

static pthread_t cb_thread;
static pthread_mutex_t cb_io_lock;
static pthread_cond_t cb_io_cond;
static struct list_head cb_ios = {.prev = &cb_ios, .next = &cb_ios};

/*
static int device_is_new(struct usbi_device *idev, unsigned short devnum)
{
  char filename[PATH_MAX + 1];
  struct stat st;

  snprintf(filename, sizeof(filename) - 1, "%s/%03d", idev->bus->filename, devnum);
  stat(filename, &st);

  if (st.st_mtime == idev->mtime)
    return 0;

  usbi_debug(1, "device %s previously existed, but mtime has changed",
	filename);

  return 0;
}
*/

static int
get_dev_descr(struct usbi_device *idev)
{
	int i, ret;
	int fd = -1;
	char ap_id[PATH_MAX + 1];
	struct usbi_device *pdev;
	char port[4], *portstr;
	struct hubd_ioctl_data ioctl_data;
	uint32_t size;
	struct usb_dev_descr *descrp;

	if ((pdev = idev->parent) == NULL) {

		goto err1;
	}

	if ((strlen(pdev->devpath) == 0) || (pdev->ap_ancestry == NULL)) {

		goto err1;
	}

	if (idev->port == 0) {

		goto err1;
	}

	port[3] = '\0';
	portstr = lltostr((long long)idev->port, &port[3]);
	sprintf(ap_id, "%s:%s%s", pdev->devpath, pdev->ap_ancestry, portstr);
	usbi_debug(3, "ap_id: %s", ap_id);

	if ((fd = open(ap_id, O_RDONLY)) == -1) {
		usbi_debug(1, "failed open device %s", ap_id);

		goto err1;
	}

	/* get device descriptor */
	descrp = malloc(USBI_DEVICE_DESC_SIZE);
	if (descrp == NULL) {
		usbi_debug(1, "unable to allocate memory for device descriptor");

		goto err2;
	}

	ioctl_data.cmd = USB_DESCR_TYPE_DEV;
	ioctl_data.port = idev->port;
	ioctl_data.misc_arg = 0;
	ioctl_data.get_size = B_FALSE;
	ioctl_data.buf = (caddr_t)descrp;
	ioctl_data.bufsiz = USBI_DEVICE_DESC_SIZE;

	if (ioctl(fd, DEVCTL_AP_CONTROL, &ioctl_data) != 0) {
		usbi_debug(1, "failed to get device descriptor");
		free(descrp);

		goto err2;
	}

	idev->desc.device_raw.len = USBI_DEVICE_DESC_SIZE;
	memcpy(&idev->desc.device, &descrp->desc,
	    sizeof (struct usb_device_desc));
	free(descrp);

	/* get config descriptors */
	if ((idev->desc.device.bNumConfigurations > USBI_MAXCONFIG) ||
	    (idev->desc.device.bNumConfigurations < 1)) {
		usbi_debug(1, "invalid config number");

		goto err2;
	}

	idev->desc.num_configs = idev->desc.device.bNumConfigurations;

	idev->desc.configs_raw = malloc(idev->desc.num_configs *
	    sizeof (struct usbi_raw_desc));
	if (idev->desc.configs_raw == NULL) {
		usbi_debug(1, "unable to allocate memory for raw config "
		    "descriptor structures");

		goto err2;
	}

	memset(idev->desc.configs_raw, 0,
	    idev->desc.num_configs * sizeof (struct usbi_raw_desc));

	idev->desc.configs = malloc(idev->desc.num_configs *
	    sizeof (struct usbi_config));
	if (idev->desc.configs == NULL) {
		usbi_debug(1, "unable to allocate memory for config "
		    "descriptors");

		goto err3;
	}

	memset(idev->desc.configs, 0,
	    idev->desc.num_configs * sizeof(struct usbi_config));

	for (i = 0; i < idev->desc.num_configs; i++) {
		struct usbi_raw_desc *cfgr = idev->desc.configs_raw + i;

		ioctl_data.cmd = USB_DESCR_TYPE_CFG;
		ioctl_data.misc_arg = i;
		ioctl_data.get_size = B_TRUE;
		ioctl_data.buf = (caddr_t)&size;
		ioctl_data.bufsiz = sizeof (size);

		if (ioctl(fd, DEVCTL_AP_CONTROL, &ioctl_data) != 0) {
			usbi_debug(1, "failed to get config descr %d size", i);

			goto err4;
		}

		cfgr->len = size;
		cfgr->data = malloc(size);
		if (cfgr->data == NULL) {
			usbi_debug(1, "failed to alloc raw config descriptor "
			    "%d", i);

			goto err4;
		}

		ioctl_data.get_size = B_FALSE;
		ioctl_data.buf = (caddr_t)cfgr->data;
		ioctl_data.bufsiz = size;

		if (ioctl(fd, DEVCTL_AP_CONTROL, &ioctl_data) != 0) {
			usbi_debug(1, "failed to get config descr %d", i);

			goto err4;
		}

		ret = usbi_parse_configuration(idev->desc.configs + i,
		    cfgr->data, cfgr->len);
		if (ret > 0) {
			usbi_debug(2, "%d bytes of descriptor data still left",
			    ret);
		} else if (ret < 0) {
			usbi_debug(1, "unable to parse descriptor %d", i);

			goto err4;
		}
	}

	return (0);

err4:
	for (i = 0; i < idev->desc.num_configs; i++) {
		if (idev->desc.configs_raw->data != NULL) {
			free(idev->desc.configs_raw->data);
		}
	}
	free(idev->desc.configs);
	idev->desc.configs = NULL;
err3:
	free(idev->desc.configs_raw);
	idev->desc.configs_raw = NULL;
err2:
	close(fd);
err1:
	return (-1);
}


/*
 * XXX: The minor nodes and devlink information will not be available
 * once the debug kernel unloads a driver module. We need to force
 * load usb drivers esp. ugen driver.
 */
static int
check_devlink(di_devlink_t link, void *arg)
{
	struct devlink_cbarg *dlarg = (struct devlink_cbarg *)arg;
	const char *str;
	char *newstr, *p;

	if ((dlarg->idev->devlink != NULL) &&
	    (dlarg->idev->ugenpath != NULL) &&
	    (dlarg->idev->ap_ancestry != NULL)) {

		return (DI_WALK_TERMINATE);
	}

	str = di_devlink_path(link);

	if ((strncmp("/dev/usb/", str, 9) != 0) &&
	    (strncmp("/dev/cfg/", str, 9) != 0)) {

		return (DI_WALK_CONTINUE);
	}

	/* check the minor node type */
	if (strcmp("ddi_ctl:attachment_point:usb",
	    di_minor_nodetype(dlarg->minor)) == 0) {
		/* cfgadm node */
		if (dlarg->idev->ap_ancestry != NULL) {

			return (DI_WALK_CONTINUE);
		}

		if ((p = malloc(APID_NAMELEN + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, APID_NAMELEN + 1);
		str = di_minor_name(dlarg->minor);
		dlarg->idev->ap_ancestry = p;

		/* retrieve cfgadm ap_id ancestry */
		if ((newstr = strrchr(str, '.')) != NULL) {
			(void) strncpy(p, str,
			    strlen(str) - strlen(newstr) + 1);
		}

		usbi_debug(3, "ap_ancestry: %s", dlarg->idev->ap_ancestry);

		return (DI_WALK_CONTINUE);
	} else if (strcmp("ddi_generic:usb",
	    di_minor_nodetype(dlarg->minor)) == 0) {
		/* ugen node */
		if (dlarg->idev->ugenpath != NULL) {

			return (DI_WALK_CONTINUE);
		}

		if ((p = malloc(PATH_MAX + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		/* retrieve ugen link path */
		if ((newstr = strrchr(str, '/')) == NULL) {
			free(p);

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, PATH_MAX + 1);
		(void) strncpy(p, str, strlen(str) - strlen(newstr));
		dlarg->idev->ugenpath = p;
		usbi_debug(3, "ugen_link: %s", dlarg->idev->ugenpath);

		return (DI_WALK_CONTINUE);
	} else {
		/* there should be only one such link */
		if ((p = malloc(PATH_MAX + 1)) == NULL) {

			return (DI_WALK_TERMINATE);
		}

		memset(p, 0, PATH_MAX + 1);
		(void) strcpy(p, str);
		dlarg->idev->devlink = p;
		usbi_debug(3, "dev_link: %s", dlarg->idev->devlink);

		return (DI_WALK_CONTINUE);
	}
}

static void
get_minor_node_link(di_node_t node, struct usbi_device *idev)
{
	di_minor_t minor = DI_MINOR_NIL;
	char *minor_path;
	struct devlink_cbarg arg;

	while ((minor = di_minor_next(node, minor)) != DI_MINOR_NIL) {
		minor_path = di_devfs_minor_path(minor);
		arg.idev = idev;
		arg.minor = minor;
		(void) di_devlink_walk(devlink_hdl, NULL, minor_path,
		    DI_PRIMARY_LINK, (void *)&arg, check_devlink);
		di_devfs_path_free(minor_path);
	}
}

static void
create_new_device(di_node_t node, struct usbi_device *pdev, struct usbi_bus *ibus)
{
	di_node_t cnode;
	struct usbi_device *idev;
	int *nport_prop, *port_prop, *addr_prop, n;
	char *phys_path;

	usbi_debug(3, "check %s%d", di_driver_name(node), di_instance(node));
	idev = (struct usbi_device *)malloc(sizeof (struct usbi_device));
	if (idev == NULL)
		return;

	memset(idev, 0, sizeof (struct usbi_device));

	if (node == ibus->node) {
		idev->devnum = 1;
		idev->parent = NULL;
	} else {
		n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
		    "assigned-address", &addr_prop);
		if ((n != 1) || (*addr_prop == 0)) {
			usbi_debug(1, "cannot get valid usb_addr");
			free(idev);

			return;
		}

		n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
		    "reg", &port_prop);
		if ((n != 1) || (*port_prop > pdev->num_ports) ||
		   (*port_prop <= 0)) {
			usbi_debug(1, "cannot get valid port index");
			free(idev);

			return;
		}

		idev->devnum = *addr_prop;
		idev->parent = pdev;
		idev->port = *port_prop;
	}

	if ((n = di_prop_lookup_ints(DDI_DEV_T_ANY, node,
	    "usb-port-number", &nport_prop)) > 1) {
		usbi_debug(1, "invalid usb-port-number");
		free(idev);

		return;
	}

	if (n == 1) {
		idev->num_ports = *nport_prop;
		idev->children = malloc(idev->num_ports *
		    sizeof (idev->children[0]));
		if (idev->children == NULL) {
			free(idev);

			return;
		}

		memset(idev->children, 0, idev->num_ports *
		    sizeof (idev->children[0]));
	} else {
		idev->num_ports = 0;
	}

	phys_path = di_devfs_path(node);
	snprintf(idev->devpath, sizeof (idev->devpath), "/devices%s",
	    phys_path);
	di_devfs_path_free(phys_path);

	get_minor_node_link(node, idev);

	if (node != ibus->node) {
		(void) get_dev_descr(idev);
	}

	if (node == ibus->node) {
		ibus->root = idev;
	} else {
		pdev->children[*port_prop-1] = idev;
	}

	ibus->dev_by_num[idev->devnum] = idev;
	usbi_add_device(ibus, idev);
	idev->found = 1;
	idev->info.ep0_fd = -1;
	idev->info.ep0_fd_stat = -1;

	usbi_debug(2, "found usb device: bus %d dev %d",
	    ibus->busnum, idev->devnum, idev->devpath);
	usbi_debug(2, "device path: %s", idev->devpath);


	if (idev->num_ports) {
		cnode = di_child_node(node);
		while (cnode != DI_NODE_NIL) {
			create_new_device(cnode, idev, ibus);
			cnode = di_sibling_node(cnode);
		}
	}
}

static int
solaris_refresh_devices(struct usbi_bus *ibus)
{
	struct usbi_device *idev, *tidev;

	/* Search devices from root-hub */
	if ((root_node = di_init(ibus->filename, DINFOCPYALL)) ==
	    DI_NODE_NIL) {
		usbi_debug(1, "di_init() failed: %s", strerror(errno));

		return (LIBUSB_FAILURE);
	}

	if ((devlink_hdl = di_devlink_init(NULL, 0)) == NULL) {
		usbi_debug(1, "di_devlink_init() failed: %s",
		    strerror(errno));
		di_fini(root_node);

		return (LIBUSB_FAILURE);
	}

	pthread_mutex_lock(&ibus->lock);

	/* Reset the found flag for all devices */
	list_for_each_entry(idev, &ibus->devices, bus_list) {
		idev->found = 0;
	}

	ibus->node = root_node;

	create_new_device(root_node, NULL, ibus);

	list_for_each_entry_safe(idev, tidev, &ibus->devices, bus_list) {
		if (!idev->found) {
			/* Device disappeared, remove it */
			usbi_debug(2, "device %d removed", idev->devnum);
			usbi_remove_device(idev);
		}
	}

	pthread_mutex_unlock(&ibus->lock);

	di_fini(root_node);
	(void) di_devlink_fini(&devlink_hdl);

	return (LIBUSB_SUCCESS);
}

static int
detect_root_hub(di_node_t node, void *arg)
{
	struct list_head *busses = (struct list_head *)arg;
	struct usbi_bus *ibus;
	uchar_t *prop_data = NULL;
	char *phys_path;

	if (di_prop_lookup_bytes(DDI_DEV_T_ANY, node, "root-hub",
	    &prop_data) != 0) {

		return (DI_WALK_CONTINUE);
	}

	ibus = (struct usbi_bus *)malloc(sizeof(*ibus));
	if (ibus == NULL) {
		usbi_debug(1, "malloc ibus failed: %s", strerror(errno));

		return (DI_WALK_TERMINATE);
	}

	memset(ibus, 0, sizeof(*ibus));

	pthread_mutex_init(&ibus->lock, NULL);

	ibus->busnum = ++busnum;
	phys_path = di_devfs_path(node);
	snprintf(ibus->filename, sizeof (ibus->filename), "%s", phys_path);
	di_devfs_path_free(phys_path);

	list_add(&ibus->list, busses);

	usbi_debug(2, "found bus %s%d:%s", di_driver_name(node),
	    di_instance(node), ibus->filename);

	return (DI_WALK_PRUNECHILD);
}

static int
solaris_find_busses(struct list_head *busses)
{
	/* Search dev_info tree for root-hubs */
	if ((root_node = di_init("/", DINFOCPYALL)) == DI_NODE_NIL) {
		usbi_debug(1, "di_init() failed: %s", strerror(errno));

		return (LIBUSB_FAILURE);
	}

	if (di_walk_node(root_node, DI_WALK_SIBFIRST, busses,
	    detect_root_hub) == -1) {
		usbi_debug(1, "di_walk_node() failed: %s", strerror(errno));
		di_fini(root_node);

		return (LIBUSB_FAILURE);
	}

	usbi_debug(3, "solaris_find_busses finished");
	di_fini(root_node);

	return (LIBUSB_SUCCESS);
}

static void
solaris_free_device(struct usbi_device *idev)
{
	if (idev->devlink) {
		free(idev->devlink);
	}

	if (idev->ugenpath) {
		free(idev->ugenpath);
	}

	if (idev->ap_ancestry) {
		free(idev->ap_ancestry);
	}
}

static int
usb_open_ep0(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;
	char filename[PATH_MAX + 1];

	if (idev->info.ep0_fd >= 0) {
		idev->info.ref_count++;
		hdev->ep_fd[0] = idev->info.ep0_fd;
		hdev->ep_status_fd[0] = idev->info.ep0_fd_stat;

		usbi_debug(2, "ep0 of dev: %s already opened", idev->devpath);

		return (0);
	}

	snprintf(filename, PATH_MAX, "%s/cntrl0", idev->ugenpath);
	usbi_debug(3, "opening %s", filename);

	hdev->ep_fd[0] = open(filename, O_RDWR);
	if (hdev->ep_fd[0] < 0) {
		usbi_debug(1, "open cntrl0 of dev: %s failed (%d)",
		    idev->devpath, errno);

		return (-1);
	}

	snprintf(filename, PATH_MAX, "%s/cntrl0stat", idev->ugenpath);
	usbi_debug(3, "opening %s", filename);

	hdev->ep_status_fd[0] = open(filename, O_RDONLY);
	if (hdev->ep_status_fd[0] < 0) {
		usbi_debug(1, "open cntrl0stat of dev: %s failed (%d)",
		    idev->devpath, errno);
		close(hdev->ep_fd[0]);
		hdev->ep_fd[0] = -1;

		return (-1);
	}

	/* allow sharing between multiple opens */
	idev->info.ep0_fd = hdev->ep_fd[0];
	idev->info.ep0_fd_stat = hdev->ep_status_fd[0];
	idev->info.ref_count++;

	usbi_debug(2, "ep0 opened");

	return (0);
}

static int
solaris_open(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;
	int i;

	if (idev->ugenpath == NULL) {
		usbi_debug(1, "open dev: %s not supported", idev->devpath);

		return (LIBUSB_FAILURE);
	}

	/* set all file descriptors to "closed" */
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		hdev->ep_fd[i] = -1;
		hdev->ep_status_fd[i] = -1;
		if (i > 0) {
			hdev->ep_interface[i] = -1;
		}
	}
	hdev->config_value = -1;
	hdev->config_index = -1;

	/* open default control ep and keep it open */
	if (usb_open_ep0(hdev) != 0) {

		return (LIBUSB_FAILURE);
	}

	return (LIBUSB_SUCCESS);
}

static int
solaris_get_configuration(struct usbi_device *idev, int *cfg)
{
	return (LIBUSB_NOT_SUPPORTED);
}

static int
solaris_set_configuration(struct usbi_device *idev, int cfg)
{
	/* may implement by cfgadm */
	return (LIBUSB_NOT_SUPPORTED);
}

/*
 * usb_ep_index:
 *	creates an index from endpoint address that can
 *	be used to index into endpoint lists
 *
 * Returns: ep index (a number between 0 and 31)
 */
static uchar_t
usb_ep_index(uint8_t ep_addr)
{
	return ((ep_addr & USB_ENDPOINT_ADDRESS_MASK) +
	    ((ep_addr & USB_ENDPOINT_DIR_MASK) ? 16 : 0));
}

/* initialize ep_interface arrays */
static void
usb_set_ep_iface_alts(struct usbi_dev_handle *hdev,
    int index, int interface, int alt)
{
	struct usbi_device *idev = hdev->idev;
	struct usbi_altsetting *as;
	struct usb_interface_desc *if_desc;
	struct usb_endpoint_desc *ep_desc;
	int i;

	/* reinitialize endpoint arrays */
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		hdev->ep_interface[i] = -1;	/* XXX: ep0? */
	}

	as = &idev->desc.configs[index].interfaces[interface].altsettings[alt];
	if_desc = &as->desc;

	for (i = 0; i < if_desc->bNumEndpoints; i++) {
		ep_desc = (struct usb_endpoint_descr *)&as->endpoints[i];
		hdev->ep_interface[usb_ep_index(
		    ep_desc->bEndpointAddress)] = interface;
	}

	usbi_debug(3, "ep_interface:");
	for (i = 0; i < USBI_MAXENDPOINTS; i++) {
		usbi_debug(3, "%d - %d ", i, hdev->ep_interface[i]);
	}
}

static int
solaris_claim_interface(struct usbi_dev_handle *hdev, int interface)
{
	struct usbi_device *idev = hdev->idev;
	int index;

	if ((idev->desc.device_raw.len == 0) || (idev->desc.configs == NULL)) {
		usbi_debug(1, "device access not supported");

		return (LIBUSB_NOACCESS);
	}

	if (hdev->config_value == -1) {
		index = 0;
	} else {
		for (index = 0; index < idev->desc.num_configs; index++) {
			if (hdev->config_value ==
			    idev->desc.configs[index].desc.bConfigurationValue) {
				break;
			}
		}
		if (index == idev->desc.num_configs) {
			usbi_debug(1, "invalid config_value %d",
			    hdev->config_value);

			return (LIBUSB_FAILURE);
		}
	}

	hdev->config_value =
	    idev->desc.configs[index].desc.bConfigurationValue;
	hdev->config_index = index;

	usbi_debug(3, "config_value = %d, config_index = %d",
	    hdev->config_value, hdev->config_index);

	/* check if this is a valid interface */
	if ((interface < 0) || (interface >= USBI_MAXINTERFACES) ||
	    (interface >= idev->desc.configs[index].num_interfaces)) {
		usbi_debug(1, "interface %d not valid", interface);

		return (LIBUSB_BADARG);
	}

	/* already claimed this interface */
	if (idev->info.claimed_interfaces[interface] == hdev) {

		return (LIBUSB_SUCCESS);
	}

	/* claimed another interface */
	if (hdev->interface != -1) {
		usbi_debug(1, "please release interface before claiming "
		    "a new one");

		return (LIBUSB_BUSY);
	}

	/* interface claimed by others */
	if (idev->info.claimed_interfaces[interface] != NULL) {
		usbi_debug(1, "this interface has been claimed by others");

		return (LIBUSB_BUSY);
	}

	hdev->interface = interface;
	hdev->altsetting = 0;
	idev->info.claimed_interfaces[interface] = hdev;

	usb_set_ep_iface_alts(hdev, index, interface, 0);

	usbi_debug(3, "interface %d claimed", interface);

	return (LIBUSB_SUCCESS);
}

static int
solaris_release_interface(struct usbi_dev_handle *hdev, int interface)
{
	struct usbi_device *idev = hdev->idev;

	if ((hdev->interface == -1) || (hdev->interface != interface)) {
		usbi_debug(1, "invalid arg");

		return (LIBUSB_BADARG);
	}

	if (idev->info.claimed_interfaces[interface] != hdev) {
		usbi_debug(1, "handle dismatch");

		return (LIBUSB_FAILURE);
	}

	idev->info.claimed_interfaces[interface] = NULL;
	hdev->interface = -1;
	hdev->altsetting = -1;

	return (LIBUSB_SUCCESS);
}

static void
usb_close_all_eps(struct usbi_dev_handle *hdev)
{
	int i;

	/* not close ep0 */
	for (i = 1; i < USBI_MAXENDPOINTS; i++) {
		if (hdev->ep_fd[i] != -1) {
			(void) close(hdev->ep_fd[i]);
			hdev->ep_fd[i] = -1;
		}
		if (hdev->ep_status_fd[i] != -1) {
			(void) close(hdev->ep_status_fd[i]);
			hdev->ep_status_fd[i] = -1;
		}
	}
}

static int
usb_close_ep0(struct usbi_dev_handle *hdev)
{
	struct usbi_device *idev = hdev->idev;

	if (idev->info.ep0_fd >= 0) {
		if (--(idev->info.ref_count) > 0) {
			usbi_debug(3, "ep0 of dev %s: ref_count=%d",
			    idev->devpath, idev->info.ref_count);

			return (0);
		}

		if ((hdev->ep_fd[0] != idev->info.ep0_fd) ||
		    (hdev->ep_status_fd[0] != idev->info.ep0_fd_stat)) {
			usbi_debug(1, "unexpected error closing ep0 of dev %s",
			    idev->devpath);

			return (-1);
		}

		close(idev->info.ep0_fd);
		close(idev->info.ep0_fd_stat);
		idev->info.ep0_fd = -1;
		idev->info.ep0_fd_stat = -1;
		hdev->ep_fd[0] = -1;
		hdev->ep_status_fd[0] = -1;
		usbi_debug(3, "ep0 of dev %s closed", idev->devpath);

		return (0);
	} else {
		usbi_debug(1, "ep0 of dev %s not open or already closed",
		    idev->devpath);

		return (-1);
	}
}

static int
solaris_close(struct usbi_dev_handle *hdev)
{
	if (hdev->interface != -1) {
		solaris_release_interface(hdev, hdev->interface);
	}

	usb_close_all_eps(hdev);
	usb_close_ep0(hdev);

	return (LIBUSB_SUCCESS);
}

static void
usb_dump_data(char *data, size_t size)
{
	int i;

	(void) fprintf(stderr, "data dump:");
	for (i = 0; i < size; i++) {
		if (i % 16 == 0) {
			(void) fprintf(stderr, "\n%08x	", i);
		}
		(void) fprintf(stderr, "%02x ", (uchar_t)data[i]);
	}
	(void) fprintf(stderr, "\n");
}

/*
 * usb_get_status:
 *	gets status of endpoint
 *
 * Returns: ugen's last cmd status
 */
static int
usb_get_status(int fd)
{
	int status, ret;

	usbi_debug(2, "usb_get_status(): fd=%d\n", fd);

	ret = read(fd, &status, sizeof (status));
	if (ret == sizeof (status)) {
		switch (status) {
		case USB_LC_STAT_NOERROR:
			usbi_debug(1, "No Error\n");
			break;
		case USB_LC_STAT_CRC:
			usbi_debug(1, "CRC Timeout Detected\n");
			break;
		case USB_LC_STAT_BITSTUFFING:
			usbi_debug(1, "Bit Stuffing Violation\n");
			break;
		case USB_LC_STAT_DATA_TOGGLE_MM:
			usbi_debug(1, "Data Toggle Mismatch\n");
			break;
		case USB_LC_STAT_STALL:
			usbi_debug(1, "End Point Stalled\n");
			break;
		case USB_LC_STAT_DEV_NOT_RESP:
			usbi_debug(1, "Device is Not Responding\n");
			break;
		case USB_LC_STAT_PID_CHECKFAILURE:
			usbi_debug(1, "PID Check Failure\n");
			break;
		case USB_LC_STAT_UNEXP_PID:
			usbi_debug(1, "Unexpected PID\n");
			break;
		case USB_LC_STAT_DATA_OVERRUN:
			usbi_debug(1, "Data Exceeded Size\n");
			break;
		case USB_LC_STAT_DATA_UNDERRUN:
			usbi_debug(1, "Less data received\n");
			break;
		case USB_LC_STAT_BUFFER_OVERRUN:
			usbi_debug(1, "Buffer Size Exceeded\n");
			break;
		case USB_LC_STAT_BUFFER_UNDERRUN:
			usbi_debug(1, "Buffer Underrun\n");
			break;
		case USB_LC_STAT_TIMEOUT:
			usbi_debug(1, "Command Timed Out\n");
			break;
		case USB_LC_STAT_NOT_ACCESSED:
			usbi_debug(1, "Not Accessed by h/w\n");
			break;
		case USB_LC_STAT_UNSPECIFIED_ERR:
			usbi_debug(1, "Unspecified Error\n");
			break;
		case USB_LC_STAT_NO_BANDWIDTH:
			usbi_debug(1, "No Bandwidth\n");
			break;
		case USB_LC_STAT_HW_ERR:
			usbi_debug(1,
			    "Host Controller h/w Error\n");
			break;
		case USB_LC_STAT_SUSPENDED:
			usbi_debug(1, "Device was Suspended\n");
			break;
		case USB_LC_STAT_DISCONNECTED:
			usbi_debug(1, "Device was Disconnected\n");
			break;
		case USB_LC_STAT_INTR_BUF_FULL:
			usbi_debug(1,
			    "Interrupt buffer was full\n");
			break;
		case USB_LC_STAT_INVALID_REQ:
			usbi_debug(1, "Request was Invalid\n");
			break;
		case USB_LC_STAT_INTERRUPTED:
			usbi_debug(1, "Request was Interrupted\n");
			break;
		case USB_LC_STAT_NO_RESOURCES:
			usbi_debug(1, "No resources available for "
			    "request\n");
			break;
		case USB_LC_STAT_INTR_POLLING_FAILED:
			usbi_debug(1, "Failed to Restart Poll");
			break;
		default:
			usbi_debug(1, "Error Not Determined %d\n",
			    status);
			break;
		}
	} else {
		status = -1;
	}

	return (status);
}

static int
usb_do_io(int fd, int stat_fd, char *data, size_t size, int flag)
{
	int error;
	int ret = -1;

	usbi_debug(2, "usb_do_io(): size=0x%x flag=%d\n", size, flag);

	if (size == 0) {

		return (0);
	}

	switch (flag) {
	case READ:
		ret = read(fd, data, size);
//		usb_dump_data(data, size);
		break;
	case WRITE:
		usb_dump_data(data, size);
		ret = write(fd, data, size);
		break;
	}
	if (ret < 0) {
		int save_errno = errno;

		/* usb_get_status will do a read and overwrite errno */
		error = usb_get_status(stat_fd);
		usbi_debug(1, "io status=%d errno=%d",error, save_errno);

		return (-save_errno);
	}

	usbi_debug(3, "usb_do_io(): amount=%d\n", ret);

	return (ret);
}

static int
solaris_submit_ctrl(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	unsigned char *data = io->ctrl.setup;
	int ret;

	if (hdev->ep_fd[0] == -1) {
		usbi_debug(1, "ep0 not opened");

		return (LIBUSB_NOACCESS);
	}

	if ((data[0] & USB_REQ_DIR_MASK) == USB_REQ_DEV_TO_HOST) {
		ret = usb_do_io(hdev->ep_fd[0], hdev->ep_status_fd[0],
		    (char *)data, USBI_CONTROL_SETUP_LEN, WRITE);
	} else {
		char *buf;

		if ((buf = malloc(io->ctrl.buflen + USBI_CONTROL_SETUP_LEN)) ==
		    NULL) {
			usbi_debug(1, "alloc for ctrl out failed");

			return (LIBUSB_NO_RESOURCES);
		}
		(void) memcpy(buf, data, USBI_CONTROL_SETUP_LEN);
		(void) memcpy(buf + USBI_CONTROL_SETUP_LEN, io->ctrl.buf,
		    io->ctrl.buflen);

		ret = usb_do_io(hdev->ep_fd[0], hdev->ep_status_fd[0],
		    buf, io->ctrl.buflen + USBI_CONTROL_SETUP_LEN, WRITE);

		free(buf);
	}

	if (ret < USBI_CONTROL_SETUP_LEN) {
		usbi_debug(1, "error sending control msg: %d", ret);

		return (LIBUSB_FAILURE);
	}

	ret -= USBI_CONTROL_SETUP_LEN;

	/* send the remaining bytes for IN request */
	if ((io->ctrl.buflen) && ((data[0] & USB_REQ_DIR_MASK) ==
	    USB_REQ_DEV_TO_HOST)) {
		ret = usb_do_io(hdev->ep_fd[0], hdev->ep_status_fd[0],
		    (char *)io->ctrl.buf, io->ctrl.buflen, READ);
	}

	usbi_debug(3, "send ctrl bytes %d", ret);

	return (ret);
}

static int
usb_check_device_and_status_open(struct usbi_dev_handle *hdev,
    uint8_t ep_addr, int ep_type)
{
	uint8_t ep_index;
	char filename[PATH_MAX + 1], statfilename[PATH_MAX + 1];
	char cfg_num[16], alt_num[16];
	int fd, fdstat, mode;

	ep_index = usb_ep_index(ep_addr);

	if ((hdev->config_value == -1) || (hdev->interface == -1) ||
	    (hdev->altsetting == -1)) {
		usbi_debug(1, "interface not claimed");

		return (EACCES);
	}

	if (hdev->interface != hdev->ep_interface[ep_index]) {
		usbi_debug(1, "ep %d not belong to the claimed interface",
		    ep_addr);

		return (EACCES);
	}

	/* ep already opened */
	if ((hdev->ep_fd[ep_index] > 0) &&
	    (hdev->ep_status_fd[ep_index] > 0)) {

		return (0);
	}

	/* create filename */
	if (hdev->config_index > 0) {
		(void) snprintf(cfg_num, sizeof (cfg_num), "cfg%d",
		    hdev->config_value);
	} else {
		(void) memset(cfg_num, 0, sizeof (cfg_num));
	}

	if (hdev->altsetting > 0) {
		(void) snprintf(alt_num, sizeof (alt_num), ".%d",
		    hdev->altsetting);
	} else {
		(void) memset(alt_num, 0, sizeof (alt_num));
	}

	(void) snprintf(filename, PATH_MAX, "%s/%sif%d%s%s%d",
	    hdev->idev->ugenpath, cfg_num, hdev->interface,
	    alt_num, (ep_addr & USB_ENDPOINT_DIR_MASK) ? "in" :
	    "out", (ep_addr & USB_ENDPOINT_ADDRESS_MASK));

	usbi_debug(3, "ep %d node name: %s", ep_addr, filename);

	(void) snprintf(statfilename, PATH_MAX, "%sstat", filename);

	/*
	 * for interrupt IN endpoints, we need to enable one xfer
	 * mode before opening the endpoint
	 */
	if ((ep_type == USB_ENDPOINT_TYPE_INTERRUPT) &&
	    (ep_addr & USB_ENDPOINT_IN)) {
		char control = USB_EP_INTR_ONE_XFER;
		int count;

		/* open the status device node for the ep first RDWR */
		if ((fdstat = open(statfilename, O_RDWR)) == -1) {
			usbi_debug(1, "can't open %s RDWR: %d",
			    statfilename, errno);
		} else {
			count = write(fdstat, &control, sizeof (control));

			if (count != 1) {
				/* this should have worked */
				usbi_debug(1, "can't write to %s: %d",
				    filename, errno);
				(void) close(fdstat);

				return (errno);
			}
			/* close status node and open xfer node first */
			close (fdstat);
		}
	} 

	/* open the xfer node first in case alt needs to be changed */
	if (ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
		mode = O_RDWR;
	} else if (ep_addr & USB_ENDPOINT_IN) {
		mode = O_RDONLY;
	} else {
		mode = O_WRONLY;
	}

	if ((fd = open(filename, mode)) == -1) {
		usbi_debug(1, "can't open %s: %d", filename, errno);

		return (errno);
	}

	/* open the status node */
	if ((fdstat = open(statfilename, O_RDONLY)) == -1) {
		usbi_debug(1, "can't open %s: %d", statfilename, errno);
		(void) close(fd);

		return (errno);
	}

	hdev->ep_fd[ep_index] = fd;
	hdev->ep_status_fd[ep_index] = fdstat;

	return (0);	
}

static int
solaris_submit_bulk(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;

	ep_addr = io->bulk.request->endpoint;
	ep_index = usb_ep_index(ep_addr);

	if ((ret = usb_check_device_and_status_open(hdev,
	    ep_addr, USB_ENDPOINT_TYPE_BULK)) != 0) {
		usbi_debug(1, "check_device_and_status_open for ep %d failed",
		    ep_addr);

		return (LIBUSB_NOACCESS);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		ret = usb_do_io(hdev->ep_fd[ep_index],
		    hdev->ep_status_fd[ep_index], (char *)io->bulk.buf,
		    io->bulk.buflen, READ);
	} else {
		ret = usb_do_io(hdev->ep_fd[ep_index],
		    hdev->ep_status_fd[ep_index], (char *)io->bulk.buf,
		    io->bulk.buflen, WRITE);
	}

	usbi_debug(3, "send bulk bytes %d", ret);

	return (ret);
}

static int
solaris_submit_intr(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;

	ep_addr = io->intr.request->endpoint;
	ep_index = usb_ep_index(ep_addr);

	if ((ret = usb_check_device_and_status_open(hdev,
	    ep_addr, USB_ENDPOINT_TYPE_INTERRUPT)) != 0) {
		usbi_debug(1, "check_device_and_status_open for ep %d failed",
		    ep_addr);

		return (LIBUSB_NOACCESS);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		ret = usb_do_io(hdev->ep_fd[ep_index],
		    hdev->ep_status_fd[ep_index], (char *)io->intr.buf,
		    io->intr.buflen, READ);

		/* close the endpoint so we stop polling the endpoint now */
		(void) close(hdev->ep_fd[ep_index]);
		(void) close(hdev->ep_status_fd[ep_index]);
		hdev->ep_fd[ep_index] = -1;
		hdev->ep_status_fd[ep_index] = -1;
	} else {
		ret = usb_do_io(hdev->ep_fd[ep_index],
		    hdev->ep_status_fd[ep_index], (char *)io->intr.buf,
		    io->intr.buflen, WRITE);
	}

	usbi_debug(3, "send intr bytes %d", ret);

	return (ret);
}

static void *
isoc_read(void *arg)
{
	struct usbi_dev_handle *hdev;
	uint8_t ep_addr, ep_index;
	struct usbi_io *io = (struct usbi_io *)arg, *newio;
	struct libusb_isoc_packet *pkt;
	int ret, i, err_count = 0;
	char *p;
	struct usbk_isoc_pkt_descr *pkt_descr;

	usbi_debug(3, "isoc_read thread started, flag=%d, err_count=%d",
	    io->isoc.request->flags, err_count);

	ep_addr = io->endpoint;
	ep_index = usb_ep_index(ep_addr);
	hdev = io->dev;

	while ((io->isoc.request->flags == 0) && (err_count < 10)) {
		/* isoc in not stopped */
		usbi_debug(3, "isoc reading ...");
		char *buf;

		if ((buf = malloc(io->isoc_io.buflen)) == NULL) {
			usbi_debug(1, "malloc buf failed");
			err_count++;

			continue;
		}

		if ((newio = usbi_alloc_io(hdev->handle, io->type,
		    io->endpoint, io->timeout)) == NULL) {
			usbi_debug(1, "malloc io failed");
			free(buf);
			err_count++;

			continue;
		}

		memcpy(&newio->isoc, &io->isoc, sizeof (io->isoc));
		newio->isoc.results = NULL;
		newio->isoc_io.buflen = io->isoc_io.buflen;
		newio->isoc_io.buf = buf;
		memset(buf, 0, newio->isoc_io.buflen);

		newio->isoc.results = (struct libusb_isoc_result *)malloc(
		    newio->isoc.num_packets *
		    sizeof (struct libusb_isoc_result));
		if (newio->isoc.results == NULL) {
			usbi_debug(1, "malloc isoc results failed");
			free(buf);
			usbi_free_io(newio);
			err_count++;

			continue;
		}

		ret = usb_do_io(hdev->ep_fd[ep_index],
		    hdev->ep_status_fd[ep_index], (char *)newio->isoc_io.buf,
		    newio->isoc_io.buflen, READ);

		if (ret < 0) {
			usbi_debug(1, "isoc read %d bytes failed",
			    newio->isoc_io.buflen);
			usbi_free_io(newio);
			free(buf);
			err_count++;

			continue;
		} else {
			usbi_debug(3, "isoc read %d bytes", ret);

			pkt = newio->isoc.request->packets;
			p = ((char *)newio->isoc_io.buf) +
			    newio->isoc.num_packets *
			    sizeof (struct usbk_isoc_pkt_descr);
			for (i = 0; i < newio->isoc.num_packets; i++) {
				memcpy(pkt[i].buf, (void *)p, pkt[i].buflen);
			}

			pkt_descr =
			    (struct usbk_isoc_pkt_descr *)newio->isoc_io.buf;
			for (i = 0; i < newio->isoc.num_packets; i++) {
				newio->isoc.results[i].status =
				    pkt_descr[i].isoc_pkt_status;
				newio->isoc.results[i].transferred_bytes =
				    pkt_descr[i].isoc_pkt_actual_length;
			}
			pthread_mutex_lock(&cb_io_lock);
			list_add(&newio->list, &cb_ios);
			pthread_cond_signal(&cb_io_cond);
			pthread_mutex_unlock(&cb_io_lock);

			err_count = 0;
		}
	}

	/* XXX: need to free buf when closing ep */
	if (io->isoc.request->flags == 1) {
		/* isoc in stopped by caller */
		usbi_free_io(io);
		(void) close(hdev->ep_fd[ep_index]);
		(void) close(hdev->ep_status_fd[ep_index]);
		hdev->ep_fd[ep_index] = -1;
		hdev->ep_status_fd[ep_index] = -1;
	} else {
		/* too many continuous errors, notify caller */
		usbi_io_complete(io, LIBUSB_FAILURE, 0);
	}

	return (NULL);
}

static int
solaris_submit_isoc(struct usbi_dev_handle *hdev, struct usbi_io *io)
{
	int ret;
	uint8_t ep_addr, ep_index;
	struct libusb_isoc_packet *packet;
	struct usbk_isoc_pkt_header *header;
	struct usbk_isoc_pkt_descr *pkt_descr;
	ushort_t n_pkt, pkt;
	uint_t pkts_len = 0, len;
	char *p, *buf;

	if (io->isoc.request->flags == 1) {
		usbi_debug(1, "wrong isoc request flags");

		return (LIBUSB_BADARG);
	}

	ep_addr = io->endpoint;
	ep_index = usb_ep_index(ep_addr);

	if ((ret = usb_check_device_and_status_open(hdev,
	    ep_addr, USB_ENDPOINT_TYPE_ISOCHRONOUS)) != 0) {
		usbi_debug(1, "check_device_and_status_open for ep %d failed",
		    ep_addr);

		return (LIBUSB_NOACCESS);
	}

	n_pkt = io->isoc.request->num_packets;
	packet = io->isoc.request->packets;
	for (pkt = 0; pkt < n_pkt; pkt++) {
		pkts_len += packet[pkt].buflen;
	}
	if (pkts_len == 0) {
		usbi_debug(1, "pkt length invalid");

		return (LIBUSB_BADARG);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		len = sizeof (struct usbk_isoc_pkt_header) +
		    sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	} else {
		len = pkts_len + sizeof (struct usbk_isoc_pkt_header)
		    + sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	}

	if ((buf = (char *)malloc(len)) == NULL) {
		usbi_debug(1, "malloc isoc out buf of length %d failed", len);

		return (LIBUSB_NO_RESOURCES);
	}

	memset(buf, 0, len);

	header = (struct usbk_isoc_pkt_header *)buf;
	header->isoc_pkts_count = n_pkt;
	header->isoc_pkts_length = pkts_len;
	p = buf + sizeof (struct usbk_isoc_pkt_header);
	pkt_descr = (struct usbk_isoc_pkt_descr *)p;
	p += sizeof (struct usbk_isoc_pkt_descr) * n_pkt;
	for (pkt = 0; pkt < n_pkt; pkt++) {
		pkt_descr[pkt].isoc_pkt_length = packet[pkt].buflen;
		pkt_descr[pkt].isoc_pkt_actual_length = 0;
		pkt_descr[pkt].isoc_pkt_status = 0;
		if ((ep_addr & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT) {
			memcpy((void *)p, packet[pkt].buf, packet[pkt].buflen);
			p += packet[pkt].buflen;
		}
	}

	/* do isoc OUT xfer or init polling for isoc IN xfer */
	ret = usb_do_io(hdev->ep_fd[ep_index],
	    hdev->ep_status_fd[ep_index], (char *)buf, len, WRITE);

	free(buf);

	if (ret < 0) {
		usbi_debug(1, "write isoc ep failed %d", ret);

		return (LIBUSB_FAILURE);
	}

	if (ep_addr & USB_ENDPOINT_DIR_MASK) {
		pthread_t thrid;

		len = sizeof (struct usbk_isoc_pkt_descr) * n_pkt + pkts_len;
		io->isoc.num_packets = n_pkt;
		io->isoc_io.buf = NULL;
		io->isoc_io.buflen = len;

		ret = pthread_create(&thrid, NULL, isoc_read, (void *)io);
		if (ret < 0) {
			usbi_debug(1, "create isoc read thread failed ret=%d",
			    ret);

			/* close the endpoint so we stop polling now */
			(void) close(hdev->ep_fd[ep_index]);
			(void) close(hdev->ep_status_fd[ep_index]);
			hdev->ep_fd[ep_index] = -1;
			hdev->ep_status_fd[ep_index] = -1;

			return (LIBUSB_FAILURE);
		}
	}
	
	return (ret);
}

static int
solaris_set_altinterface(struct usbi_dev_handle *hdev, int alt)
{
	struct usbi_device *idev = hdev->idev;
	int index, iface;

	if (hdev->interface == -1) {
		usbi_debug(1, "interface not claimed");

		return (LIBUSB_BADARG);
	}

	if (idev->info.claimed_interfaces[hdev->interface] != hdev) {
		usbi_debug(1, "handle dismatch");

		return (LIBUSB_FAILURE);
	}

	if (alt == hdev->altsetting) {
		usbi_debug(1, "same alt, no need to change");

		return (0);
	}

	usb_close_all_eps(hdev);

	iface = hdev->interface;
	index = hdev->config_index;
	if ((alt < 0) || (alt >= idev->
	    desc.configs[index].interfaces[iface].num_altsettings)) {
		usbi_debug(1, "invalid alt");

		return (LIBUSB_BADARG);
	}

	/* set alt interface is implicitly done when endpoint is opened */
	hdev->altsetting = alt;

	usb_set_ep_iface_alts(hdev, index, iface, alt);

	return (LIBUSB_SUCCESS);
}

static int
solaris_io_cancel(struct usbi_io *io)
{
	return (LIBUSB_NOT_SUPPORTED);
}

static void *
polling_cbs(void *arg)
{
	pthread_mutex_lock(&cb_io_lock);
	while (1) {
		struct list_head *tmp;
		char *buf;

		pthread_cond_wait(&cb_io_cond, &cb_io_lock);

		tmp = cb_ios.next;
		while (tmp != &cb_ios) {
			struct usbi_io *io;
			io = list_entry(tmp, struct usbi_io, list);
			buf = io->isoc_io.buf;
			list_del(&io->list);
			pthread_mutex_unlock(&cb_io_lock);
			usbi_debug(3, "received a cb");
			usbi_io_complete(io, 0, 0);
			if (buf != NULL) {
				free(buf);
			}

			pthread_mutex_lock(&cb_io_lock);
			tmp = cb_ios.next;
		}
	}

	return (NULL);
}

static int
solaris_init()
{
	int ret;

	ret = pthread_mutex_init(&cb_io_lock, NULL);
	if (ret < 0) {
		usbi_debug(1, "initing mutex failed(ret = %d)", ret);

		return (LIBUSB_FAILURE);
	}

	ret = pthread_cond_init(&cb_io_cond, NULL);
	if (ret < 0) {
		usbi_debug(1, "initing cond failed(ret = %d)", ret);
		pthread_mutex_destroy(&cb_io_lock);

		return (LIBUSB_FAILURE);
	}

	ret = pthread_create(&cb_thread, NULL, polling_cbs, NULL);
	if (ret < 0) {
		usbi_debug(1, "unable to create polling callback thread"
		    "(ret = %d)", ret);
		pthread_cond_destroy(&cb_io_cond);
		pthread_mutex_destroy(&cb_io_lock);

		return (LIBUSB_FAILURE);
	}

	return (LIBUSB_SUCCESS);
}

int backend_version = 1;
int backend_io_pattern = PATTERN_SYNC;

struct usbi_backend_ops backend_ops = {
  .init				= solaris_init,
  .find_busses			= solaris_find_busses,
  .refresh_devices		= solaris_refresh_devices,
  .free_device			= solaris_free_device,
  .dev = {
    .open			= solaris_open,
    .close			= solaris_close,
    .set_configuration		= solaris_set_configuration,
    .get_configuration		= solaris_get_configuration,
    .claim_interface		= solaris_claim_interface,
    .release_interface		= solaris_release_interface,
    .get_altinterface		= NULL,
    .set_altinterface		= solaris_set_altinterface,
    .reset			= NULL,
    .get_driver_np		= NULL,
    .attach_kernel_driver_np	= NULL,
    .detach_kernel_driver_np	= NULL,
    .submit_ctrl		= solaris_submit_ctrl,
    .submit_intr		= solaris_submit_intr,
    .submit_bulk		= solaris_submit_bulk,
    .submit_isoc		= solaris_submit_isoc,
    .io_cancel			= solaris_io_cancel,
  },
};
