/*
 * OpenUSB test program 
 *
 * Copyright (c) 2007 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libusb.h>

/*
 * This test program depends heavily on EZUSB FX2. Some of the
 * firmware can be found in CYPRESS's development kit or from
 * its website.
 *
 * But the basic test doesn't require any special hardware.
 */


/* Global data */
libusb_handle_t libhandle;
libusb_devid_t *devids;
uint32_t devnum;
uint32_t busnum;
libusb_busid_t *bus;
libusb_transfer_type_t testtype = 0;
int testloop = 0;
int testmulti = 0;
int testsync = 0;
int testasync = 0;

#define ISOC_PKT_NUM 12
#define ISOC_PKT_LEN 128

void print_endpoint(struct usb_endpoint_desc *ep)
{ 
	printf("      bEndpointAddress: %02xh\n", ep->bEndpointAddress);
	printf("      bmAttributes:     %02xh\n", ep->bmAttributes);
	printf("      wMaxPacketSize:   %d\n", ep->wMaxPacketSize);
	printf("      bInterval:        %d\n", ep->bInterval);
	printf("      bRefresh:         %d\n", ep->bRefresh);
	printf("      bSynchAddress:    %d\n", ep->bSynchAddress);
	printf("\n");
}

void print_interface(libusb_devid_t devid, int cfgidx, int ifcidx,int alt,
		struct usb_interface_desc *intf)
{ 
	int i;
	int ret;

	printf("    Interface:		%d\n", ifcidx);
	printf("    bInterfaceNumber:   %d\n", intf->bInterfaceNumber);
	printf("    bAlternateSetting:  %d\n", intf->bAlternateSetting);
	printf("    bNumEndpoints:      %d\n", intf->bNumEndpoints);
	printf("    bInterfaceClass:    %02x\n", intf->bInterfaceClass);
	printf("    bInterfaceSubClass: %02x\n", intf->bInterfaceSubClass);
	printf("    bInterfaceProtocol: %02x\n", intf->bInterfaceProtocol);
	printf("    iInterface:         %d\n", intf->iInterface);
	printf("\n");

	for (i = 0; i < intf->bNumEndpoints; i++) {
		struct usb_endpoint_desc ep;

		ret = libusb_parse_endpoint_desc(libhandle,devid,NULL,0,
			cfgidx,ifcidx,alt,i,&ep);
		if(ret != 0) {
			printf("parse endpoint desc fail, ret = %d %s\n", 
					ret, libusb_strerror(ret));
			return;
		}

		print_endpoint(&ep);
	}
}

void print_configuration(libusb_devid_t devid, int cfgidx,
		struct usb_config_desc *cfg)
{
	int i;
	int ret;

	printf("  Config:		%d\n", cfgidx);
	printf("  wTotalLength:         %d\n", cfg->wTotalLength);
	printf("  bNumInterfaces:       %d\n", cfg->bNumInterfaces);
	printf("  bConfigurationValue:  %d\n", cfg->bConfigurationValue);
	printf("  iConfiguration:       %d\n", cfg->iConfiguration);
	printf("  bmAttributes:         %02xh\n", cfg->bmAttributes);
	printf("  MaxPower:             %d\n", cfg->bMaxPower);
	printf("\n");

	for (i = 0; i < cfg->bNumInterfaces; i++) {
		struct usb_interface_desc intf;
		int j;

		for (j = 0;; j++) {
		/* no clue of how many altsettings here */
			ret = libusb_parse_interface_desc(libhandle,
				devid, NULL, 0, cfgidx, i, j, &intf);

			if(ret != 0) {
				break;
			}

			print_interface(devid, cfgidx, i, j, &intf);
		}
	}
}

/* print device descriptors */
void print_device(libusb_devid_t devid, int indent)
{
	struct usb_device_desc dev;
	char *buf=NULL;
	uint16_t buflen = 0;
	int i, ret;

	printf("\n%.*s+ device #%d\n", indent * 2, "                ",
			(int)devid);

	ret = libusb_parse_device_desc(libhandle,devid, NULL, 0,&dev);
	if(ret != 0) {
		return;
	}

	printf("bcdUSB:                 %04xh\n", dev.bcdUSB);
	printf("bDeviceClass:           %02x\n", dev.bDeviceClass);
	printf("bDeviceSubClass:        %02x\n", dev.bDeviceSubClass);
	printf("bDeviceProtocol:        %02x\n", dev.bDeviceProtocol);
	printf("bMaxPacketSize0:        %02x\n", dev.bMaxPacketSize0);
	printf("idVendor:               %04xh\n", dev.idVendor);
	printf("idProduct:              %04xh\n", dev.idProduct);
	printf("bcdDevice:              %04xh\n", dev.bcdDevice);
	printf("iManufacturer:          %d\n", dev.iManufacturer);
	printf("iProduct:               %d\n", dev.iProduct);
	printf("iSerialNumber:          %d\n", dev.iSerialNumber);
	printf("bNumConfigurations:     %d\n", dev.bNumConfigurations);
	printf("\n");
	
	for (i = 0; i < dev.bNumConfigurations; i++) {
		struct usb_config_desc cfg;

		ret = libusb_parse_config_desc(libhandle, devid, buf,
				buflen, i, &cfg);
		if(ret != 0) {
			printf("parse config desc fail: %s\n",
				libusb_strerror(ret));
			return;
		}

		print_configuration(devid, i, &cfg);
	}

	printf("\n");
}


void event_cb(libusb_handle_t handle, libusb_devid_t devid,
		libusb_event_t event, void *arg)
{
	printf("CALLBACK: lib(%llu) device(%llu) get a event(%d) with arg=%p\n",
			handle, devid, event, arg);
}

int convert_string(char *buf, usb_string_desc_t *st, int buflen)
{
	int di, si;
	unsigned char *tbuf = (unsigned char *)st;

	for (di = 0, si = 2; si < tbuf[0]; si += 2) {
		if (di >= (buflen - 1)) {
			break;
		}

		if (tbuf[si + 1]) {
			buf[di++] = '?';
		} else {
			buf[di++] = tbuf[si];
		}
	}

	buf[di] = 0;

	return di;
}

void dump_dev_data(libusb_dev_data_t *pdev)
{
	struct usb_device_desc *pdesc;
	int i;
	char buf[256];

	if(!pdev) {
		printf("Null dev\n");
	}

	printf("busid: 0x%x, devid: 0x%x, bus_addr:0x%x, pdevid: 0x%x, "
		"pport: %d, nports: %d\n", (int)pdev->busid, (int)pdev->devid,
		pdev->bus_address, (int)pdev->pdevid, pdev->pport,
		pdev->nports);

	printf("sys_path: %s\n", pdev->sys_path);
	printf("bus_path: %s\n", pdev->bus_path);

	pdesc = &pdev->dev_desc;	

	printf("Device descriptor:\n");
	printf("\tclass:%02x subclass:%02x vid:%04hx pid:%04hx\n",
			pdesc->bDeviceClass,
			pdesc->bDeviceSubClass,pdesc->idVendor,
			pdesc->idProduct);
	printf("\n");

	printf("Config descriptor:\n");
	printf("\ttype:0x%02x len=%d totalLen = %d\n",
		pdev->cfg_desc.bDescriptorType, pdev->cfg_desc.bLength,
		pdev->cfg_desc.wTotalLength);

	if (pdev->manufacturer) {
		convert_string(buf, pdev->manufacturer, 256);
		printf("manufacturer: %s\n", buf);
	}

	if (pdev->product) {
		convert_string(buf, pdev->product, 256);
		printf("prod: %s\n",buf);
	}
	if (pdev->serialnumber) {
		convert_string(buf, pdev->serialnumber, 256);
		printf("serial: %s\n", buf);
	}

	printf("MAX Xfer size:\n");
	printf("CTRL = 0x%x, INTR = 0x%x, BULK = 0x%x, ISOC = 0x%x\n",
		pdev->ctrl_max_xfer_size, pdev->intr_max_xfer_size,
		pdev->bulk_max_xfer_size, pdev->isoc_max_xfer_size);
	
	printf("\nRAW descriptor:\n");
	if(pdev->raw_cfg_desc) {
		for(i = 0; i < pdev->cfg_desc.wTotalLength; i++) {
			if ((i%16) == 0) {
				printf("\n");
			}
			printf("%02x ",pdev->raw_cfg_desc[i]);
		}
	}

	printf("\n");
}

#define CTRL_LEN 0xab

/* test SYNC control xfer */
int test_ctrl_sync(libusb_dev_handle_t devh)
{
	libusb_ctrl_request_t ctrl;
	int ret;
	int i;

	memset(&ctrl, 0 ,sizeof(libusb_ctrl_request_t));

	ctrl.setup.bmRequestType = 0x80;
	ctrl.setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	ctrl.setup.wValue = USB_DESC_TYPE_CONFIG<<8 | 0x01;
	ctrl.setup.wIndex = 0;

	ctrl.length = CTRL_LEN;
	ctrl.payload = malloc(ctrl.length);
	if(!ctrl.payload) {
		printf("malloc fail\n");
		return -1;
	}
	
	memset(ctrl.payload, 0, CTRL_LEN);
	ret = libusb_ctrl_xfer(devh, 0, 0, &ctrl);
	if (ret != 0) {
		libusb_free_devid_list(devids);
		printf("ctrl xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	printf("CONTROL: result.status = %d, xfer_bytes=%d\n",
		ctrl.result.status, ctrl.result.transferred_bytes);

	printf("CONTROL TEST DATA:\n");
	for(i = 0; i < CTRL_LEN; i++) {
		if(i%16 == 0)
			printf("\n");
		printf("%02x ",(unsigned char)ctrl.payload[i]);
	}
	printf("\n");
	if (ctrl.result.status == 0) {
		printf("libusb_ctrl_xfer: PASS\n");
	}
	free(ctrl.payload);

	return 0;

}

#define BULK_DATA_LEN 128

/* 
 * Use EZ-USB FX2 as the test device
 * load bulkloop firmware
 */
int test_bulk_sync(libusb_handle_t devh)
{
	char bulkdata[BULK_DATA_LEN];
	char bulkrd[BULK_DATA_LEN];
	libusb_bulk_request_t bulk;
	int i,ret;

	printf("Test BULK sync:\n");

	memset(&bulk, 0, sizeof(bulk));
	memset(bulkrd, 0, BULK_DATA_LEN);

	for(i = 0; i< BULK_DATA_LEN; i++) {
		bulkdata[i] = i;
	}

	bulk.payload = bulkdata;
	bulk.length = BULK_DATA_LEN;
	bulk.timeout = 10;

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_altsetting(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	/* Write BULK */
	ret = libusb_bulk_xfer(devh, 0, 2, &bulk);

	if (ret != 0) {
		printf("BULK sync xfer test fail:%s\n", libusb_strerror(ret));
		return -1;
	}
	printf("bulk sync xfer result.status = %d,xfer_bytes=%d, ret=%d\n",
		bulk.result.status, bulk.result.transferred_bytes,ret);

	/* READ BULK */
	bulk.payload = bulkrd;
	ret = libusb_bulk_xfer(devh, 0, 0x86, &bulk);
	if (ret != 0) {
		printf("bulk sync xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	printf("\nBULK DATA:\n");

	for(i = 0;i < BULK_DATA_LEN; i++) {
		if(i%16 == 0)
			printf("\n");
		printf("%02x ",(unsigned char)bulkrd[i]);
	}

	printf("\n");

	/* this can be enhanced to check data integrity */

	printf("BULK SYNC xfer test: PASS\n");
	
	return 0;
}

#define LOOP	1
/*
 * test INTERRUPT sync xfer
 * Use EZ-USB FX2 as the test device.
 * Load intrsrc or intrloop firmware
 */
int test_intr_sync(libusb_handle_t devh, int flag)
{
	char bulkdata[BULK_DATA_LEN];
	char bulkrd[BULK_DATA_LEN];
	libusb_intr_request_t intr;
	int i,ret;

	printf("Test INTR sync:\n");

	memset(&intr, 0, sizeof(intr));

	for(i = 0; i< BULK_DATA_LEN; i++) {
		bulkdata[i] = i;
	}

	intr.payload = bulkdata;
	intr.length = BULK_DATA_LEN;
	intr.timeout = 10;

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_altsetting(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	/* Write INTR first */
	if (flag == LOOP) {
		ret = libusb_intr_xfer(devh, 0, 2, &intr);

		if (ret != 0) {
			printf("xfer fail:%s\n", libusb_strerror(ret));
			return -1;
		}

		printf("intr result.status = %d,xfer_bytes=%d, ret = %d\n",
			intr.result.status, intr.result.transferred_bytes, ret);
	}

	/* READ INTR */
	intr.payload = bulkrd;
	ret = libusb_intr_xfer(devh, 0, 0x86, &intr);
	
	if (ret != 0) {
		printf("intr xfer sync fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	printf("result.status = %d,xfer_bytes=%d, ret=%d\n",intr.result.status,
		intr.result.transferred_bytes,ret);

	printf("\nINTR DATA:\n");
	
	/* can be enhanced to check data integrity */
	for(i = 0;i < BULK_DATA_LEN; i++) {
		if (i%16 == 0) {
			printf("\n");
		}
		printf("%02x ", (unsigned char)bulkrd[i]);
	}

	printf("\n");
	
	return 0;
}

/*
 * Test ISOC sync xfer
 * Use EZ-USB FX2 as test device. Load isoc firmware
 */
int test_isoc_sync(libusb_handle_t devh)
{
	char bulkdata[ISOC_PKT_NUM*ISOC_PKT_LEN];
	char bulkrd[ISOC_PKT_NUM*ISOC_PKT_LEN];
	libusb_isoc_request_t isoc;
	int i,ret;

	printf("Test ISOC sync:\n");

	memset(&isoc, 0, sizeof(isoc));
	memset(bulkrd, 0, ISOC_PKT_NUM*ISOC_PKT_LEN);

	for(i = 0; i < ISOC_PKT_NUM*ISOC_PKT_LEN; i++) {
		bulkdata[i] = i;
	}

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_altsetting(devh,0,3);/* alt 3, depends on the fw */
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	/* READ ISOC */
	isoc.isoc_results=malloc(sizeof(struct libusb_request_result) * ISOC_PKT_NUM);
	memset(isoc.isoc_results, 0, 
			sizeof(struct libusb_request_result) * ISOC_PKT_NUM);

	isoc.pkts.packets = malloc(sizeof(struct libusb_isoc_packet) *
			ISOC_PKT_NUM);

	for(i = 0;i < ISOC_PKT_NUM;i++) {
		isoc.pkts.packets[i].length = ISOC_PKT_LEN;
		isoc.pkts.packets[i].payload = bulkrd+ISOC_PKT_LEN*i;
	}
	isoc.pkts.num_packets = ISOC_PKT_NUM;
	ret = libusb_isoc_xfer(devh, 0, 0x82, &isoc); /* in */

	if (ret != 0) {
		printf("ISOC xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	printf("\nISOC DATA:\n");

	for(i=0;i<ISOC_PKT_NUM;i++) {
		int j;

		printf("ISOC packet: %d STATUS\n",i);
		printf("\tstatus=%d\n",isoc.isoc_results[i].status);
		printf("\tTbytes=%d\n",isoc.isoc_results[i].transferred_bytes);

		printf("\n");

		printf("ISOC packet: %d DATA\n",i);
		for(j =0;j<ISOC_PKT_LEN;j++) {
			if(j%16==0)
				printf("\n");
			printf("%02x ",isoc.pkts.packets[i].payload[j]);
		}
		printf("\n");

	}

	printf("\n");

	return 0;
}

int async_xfer_ctrl_test(libusb_dev_handle_t devh)
{
	char bulkdata[BULK_DATA_LEN];
	char bulkrd[BULK_DATA_LEN];
	libusb_ctrl_request_t ctrl;
	int i,ret;
	libusb_request_handle_t req;
	libusb_request_handle_t completed;

	memset(&ctrl, 0, sizeof(ctrl));
	for(i = 0; i< BULK_DATA_LEN; i++) {
		bulkdata[i] = i;
	}

	ctrl.setup.bmRequestType = 0x80;
	ctrl.setup.bRequest = USB_REQ_GET_DESCRIPTOR;
	ctrl.setup.wValue = USB_DESC_TYPE_CONFIG << 8 | 0x01;
	ctrl.setup.wIndex = 0;

	ctrl.length = CTRL_LEN;
	ctrl.payload = malloc(ctrl.length);
	if(!ctrl.payload) {
		printf("malloc fail\n");
		return -1;
	}

	req = (libusb_request_handle_t)
		malloc(sizeof(struct libusb_request_handle));
	memset(req, 0, sizeof(*req));

	req->dev = devh;
	req->interface = 0;
	req->endpoint = 0x00;
	req->type = USB_TYPE_CONTROL;

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_altsetting(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n",devids[0],
				libusb_strerror(ret));
		return -1;
	}

	req->req.ctrl = &ctrl;

	ret = libusb_xfer_aio(req);
	if (ret != 0) {
		printf("xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	/* waiting */
	ret = libusb_wait(1, &req, &completed);
	if(ret < 0) {
		printf("Ctrl async xfer fail: %s\n",libusb_strerror(ret));
		return -1;
	}
	printf("ASYNC xfer write\n");
	printf("ctrl result.status = %d, xfer_bytes=%d, ret=%d\n",
		completed->req.ctrl->result.status, 
		completed->req.ctrl->result.transferred_bytes, ret);

	req->endpoint = 0x00;
	ctrl.payload = bulkrd;
	req->req.ctrl = &ctrl;
	ret = libusb_xfer_aio(req);
	if(ret !=0 )
		printf("result.status = %d,xfer_bytes=%d, ret=%d\n",
			ctrl.result.status, ctrl.result.transferred_bytes,
			ret);

	if (ret != 0) {
		printf("Ctrl async xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

	/* polling */
	while(1) {
		ret = libusb_poll(1, &req, &completed);
		if (ret != 0) {
			printf("async xfer poll:%s\n", libusb_strerror(ret));
			return -1;
		}

		if (completed != NULL) {
			printf("Polling a data\n");
			break;
		}

		printf("Polling......\n");
		sleep(1);
	}

	printf("\nCTRL ASYNC DATA:\n");

	for(i = 0;i < BULK_DATA_LEN; i++) {

		if(i%16 == 0) {
			printf("\n");
		}

		printf("%02x ",
			(unsigned char)completed->req.ctrl->payload[i]);
	}
	printf("\n");

	return 0;
}

/*
 * Async xfer test for INTR, BULK, ISOC
 * Load different firmware accordingly
 */
int async_xfer_test(libusb_handle_t devh, libusb_transfer_type_t type, int flag)
{
	char bulkdata[BULK_DATA_LEN];
	char bulkrd[BULK_DATA_LEN];
	char isocrd[ISOC_PKT_NUM*ISOC_PKT_LEN];

	libusb_bulk_request_t bulk;
	libusb_intr_request_t intr;
	libusb_isoc_request_t isoc;
	int i;
	int ret = -1;
	int count;
	libusb_request_handle_t req;
	libusb_request_handle_t req1;
	libusb_request_handle_t reqs[3];
	libusb_request_handle_t completed;
	int loopcnt;
	
	if (type != USB_TYPE_BULK
		&& type != USB_TYPE_INTERRUPT
		&& type != USB_TYPE_ISOCHRONOUS) {
		/* just return success */
		return 0;
	}

	count = 0;

	memset(&bulk, 0, sizeof(bulk));
	memset(&intr, 0, sizeof(intr));
	memset(&isoc, 0, sizeof(isoc));

	for(i = 0; i< BULK_DATA_LEN; i++) {
		bulkdata[i] = i+2;
	}

	for(i = 0; i < ISOC_PKT_NUM*ISOC_PKT_LEN; i++) {
		isocrd[i] = i+2;
	}

	req = (libusb_request_handle_t )malloc(sizeof(*req));
	req1 = (libusb_request_handle_t )malloc(sizeof(*req1));

	memset(req, 0, sizeof(*req));
	memset(req1, 0, sizeof(*req));

	req->dev = devh;
	req->interface = 0;
	req->endpoint = 0x02;
	req->type = type;

	memset(&bulk,0,sizeof(libusb_bulk_request_t));
	bulk.payload = bulkdata;
	bulk.length = BULK_DATA_LEN;
	bulk.timeout = 0;

	intr.payload = bulkdata;
	intr.length = BULK_DATA_LEN;

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n", devids[0],
				libusb_strerror(ret));
		return -1;
	}

	if (type == USB_TYPE_BULK || type == USB_TYPE_INTERRUPT) {
		ret = libusb_set_altsetting(devh, 0, 0);
	} else if (type == USB_TYPE_ISOCHRONOUS) {
		ret = libusb_set_altsetting(devh, 0, 3);
	}

	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n", devids[0],
				libusb_strerror(ret));
		return -1;
	}

	/* Write */
	if (flag == LOOP) {
		if (type == USB_TYPE_BULK) {
			req->req.bulk = &bulk;
			ret = libusb_xfer_aio(req);
		} else if (type == USB_TYPE_INTERRUPT) {
			req->req.intr = &intr;
			ret = libusb_xfer_aio(req);
		} else if (type == USB_TYPE_ISOCHRONOUS) {
			int pktsize = sizeof(struct libusb_isoc_packet);
			isoc.pkts.packets = malloc(pktsize * ISOC_PKT_NUM);

			for(i = 0;i < ISOC_PKT_NUM;i++) {
				isoc.pkts.packets[i].length = ISOC_PKT_LEN;
				isoc.pkts.packets[i].payload =
					isocrd + ISOC_PKT_LEN * i;
			}
			isoc.pkts.num_packets = ISOC_PKT_NUM;

			isoc.isoc_results = malloc(
					sizeof(struct libusb_request_result) *
					ISOC_PKT_NUM);

			if(!isoc.isoc_results) 
				return -1;

			memset(isoc.isoc_results, 0, 
					sizeof(struct libusb_request_result) *
					ISOC_PKT_NUM);

			req->req.isoc = &isoc;
			req->interface = 0;
			req->endpoint = 0x82;
			ret = libusb_xfer_aio(req);

		} else {
			ret = -1;
		}

		switch(type) {
			case USB_TYPE_BULK:
				printf("bulk result.status=%d,"
					"xfer_bytes=%d,ret=%d\n",
					bulk.result.status,
					bulk.result.transferred_bytes, ret);
				break;
			case USB_TYPE_INTERRUPT:
				printf("intr result.status=%d,"
					"xfer_bytes=%d,ret=%d\n",
					intr.result.status,
					intr.result.transferred_bytes, ret);
				break;
			default:
				break;
		}

		if (ret != 0) {
			printf("xfer fail:%s\n", libusb_strerror(ret));
			return -1;
		}

		ret = libusb_wait(1, &req, &completed);
		if(ret < 0) {
			printf("Async xfer fail: %s\n",libusb_strerror(ret));
			return -1;
		}
		printf("ASYNC xfer write\n");
	}

	/* READ */
	switch(type) {
		case USB_TYPE_BULK:
			req->endpoint = 0x86;
			bulk.payload = bulkrd;
			req->req.bulk = &bulk;
			ret = libusb_xfer_aio(req);
			break;

		case USB_TYPE_INTERRUPT:
			intr.payload = bulkrd;
			req->req.intr = &intr;
			req->endpoint = 0x86;
			ret = libusb_xfer_aio(req);
			if (ret != 0) {
				return -1;
			}

			memcpy(req1, req, sizeof(struct libusb_request_handle));
			ret = libusb_xfer_aio(req1);

			break;

		case USB_TYPE_ISOCHRONOUS:
			isoc.pkts.packets =
				malloc(sizeof(struct libusb_isoc_packet) *
						ISOC_PKT_NUM);

			for(i = 0;i < ISOC_PKT_NUM;i++) {
				isoc.pkts.packets[i].length = ISOC_PKT_LEN;
				isoc.pkts.packets[i].payload =
					isocrd+ISOC_PKT_LEN*i;
			}

			isoc.pkts.num_packets = ISOC_PKT_NUM;

			isoc.isoc_results =
				calloc(sizeof(struct libusb_request_result) *
						ISOC_PKT_NUM, 1);

			if (!isoc.isoc_results) 
				return -1;

			req->req.isoc = &isoc;
			req->interface = 0;
			req->endpoint = 0x82;

			ret = libusb_xfer_aio(req);
			if(ret != 0 ) {
				printf("ret=%d(%s)\n", ret,
						libusb_strerror(ret));
			}

			memcpy(req1, req, sizeof(struct libusb_request_handle));

			ret = libusb_xfer_aio(req1);
			if(ret !=0 ) {
				printf("ret=%d(%s)\n", ret,
						libusb_strerror(ret));
			}

			break;

		default:
			ret = -1;
			break;
	}


	if (ret != 0) {
		printf("async xfer fail:%s\n", libusb_strerror(ret));
		return -1;
	}

#if 0 // wait
	ret = libusb_wait(1, &req, &completed);
#endif

	reqs[0]=req;
	reqs[1]=req1;
	reqs[2]=NULL;
	i = 0;
	printf("req1=%p, req2=%p\n",reqs[0],reqs[1]);
	
	if (flag == LOOP)
		loopcnt = 1;
	else
		loopcnt = 2;

	while(count < loopcnt) {
		ret = libusb_poll(loopcnt, reqs, &completed);
		if (ret != 0) {
			printf("async xfer poll:%s\n", libusb_strerror(ret));
			return -1;
		}
		if (completed == NULL) {
			printf("Polling no data\n");
			sleep(1);
			continue;
		}
		printf("Polling...... %d\n",count);
		sleep(1);

		count++;

		printf("\nINTR/BULK/ISOC ASYNC DATA:\n");

		if(type == USB_TYPE_INTERRUPT || type == USB_TYPE_BULK) {
			unsigned char *p;

			if (type == USB_TYPE_INTERRUPT) {
				p = (unsigned char *)
					completed->req.intr->payload;
			} else {
				p = (unsigned char *)
					completed->req.bulk->payload;
			}

			for(i = 0; i < BULK_DATA_LEN; i++) {
				if((i+1)%16 == 0)
					printf("\n");
				printf("%02x ", p[i]);
			}

		} else if (type == USB_TYPE_ISOCHRONOUS) {
			int j;

			for(i=0;i<ISOC_PKT_NUM;i++) {
				char *p;
				unsigned char status;
				unsigned int bytes;

				if(!completed){
					printf("NULL completed\n");
					break;
				}

				printf("\nISOC packet: %d STATUS\n",i);
				status = completed->req.isoc->
					isoc_results[i].status;
				printf("\tstatus=%d\n", status);

				bytes = completed->req.isoc->
					isoc_results[i].transferred_bytes;
				printf("\tTbytes=%d\n",bytes);

				printf("\n");

				printf("ISOC packet: %d DATA\n",i);

				p = completed->req.isoc->
					pkts.packets[i].payload;

				for(j=0;j<ISOC_PKT_LEN;j++) {
					if(j%16==0)
						printf("\n");
					printf("%02x ", (unsigned char)p[j]);
				}
			}
		}
	}
	printf("\n");
	
	return 0;
}



uint32_t m_len[ISOC_PKT_NUM];
uint8_t *m_buffers[ISOC_PKT_NUM];

int32_t multi_callback(struct libusb_multi_request_handle *mreq,
	uint32_t bufidx, libusb_request_result_t *result)
{
	int i;

	printf("Multi Request Callback:\n");
	printf("Result: status=%d size=%d\n",
		result->status, result->transferred_bytes);

	printf("bufidx = %d\n", bufidx);

	for(i=0;i<result->transferred_bytes;i++) {
		if(i%16==0) printf("\n");
		printf("%02x ",
			(uint8_t)mreq->req.intr->payloads[bufidx][i]);
	}

	printf("\n\n");

	return 0;

}

/*
 * Not mature
 */
int multi_xfer_test(libusb_dev_handle_t devh)
{
	int ret = 0;
	int i;
	libusb_multi_request_handle_t m_req;
	struct libusb_multi_intr_request *m_intr;

	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n", devids[0],
				libusb_strerror(ret));
		return -1;
	}

	ret = libusb_set_altsetting(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n", devids[0],
				libusb_strerror(ret));
		return -1;
	}

	m_req = malloc(sizeof(*m_req));
	if(!m_req) {
		printf("Resource not available\n");
		return -1;
	}

	m_intr = malloc(sizeof(*m_intr));
	if(!m_intr) {
		printf("Resource not available\n");
		return -1;
	}
	
	memset(m_req, 0, sizeof(*m_req));
	memset(m_intr, 0, sizeof(*m_intr));

	for(i=0;i<ISOC_PKT_NUM;i++) {
		m_len[i] = BULK_DATA_LEN;
	}

	
	for(i=0;i<ISOC_PKT_NUM;i++) {
		m_buffers[i] = calloc(BULK_DATA_LEN,1);
	}

	m_intr->payloads = m_buffers;

	m_intr->lengths = m_len;
	m_intr->num_bufs = ISOC_PKT_NUM;
	m_intr->timeout = 100;

	m_req->dev = devh;
	m_req->interface = 0;
	m_req->endpoint = 0x86;
	m_req->type = USB_TYPE_INTERRUPT;
	m_req->req.intr = m_intr;
	m_req->cb = multi_callback;
#if 1
	ret = libusb_start(m_req);
	if (ret !=0) {
		printf("libusb_start fail\n");
		return -1;
	}
#endif

	return ret;
}

int test_get_device_data()
{
	int ret;
	libusb_dev_data_t *devdata;
	int i,j;

	for(j=0;j<busnum;j++) {
		ret = libusb_get_devids_by_bus(libhandle, bus[j], &devids,
				&devnum);
		if(ret < 0) {
			printf("Error get devids by bus:%s\n",
					libusb_strerror(ret));
			return -1;
		}
		for(i=0;i<devnum;i++) {
			print_device(devids[i], 4);

			ret = libusb_get_device_data(libhandle, devids[i], 0,
					&devdata);
			if (ret < 0) {
				printf("Get device(%d) data error:%s\n", i,
						libusb_strerror(ret));
				return -1;
			}
			
			dump_dev_data(devdata);

			libusb_free_device_data(devdata);

		}
		libusb_free_devid_list(devids);
	}
	
	return 0;
}

int test_sync_xfer(libusb_dev_handle_t devh)
{
	int ret;

	ret = test_ctrl_sync(devh);
	if (ret != 0) {
		printf("CONTROL xfer fail\n");
		return -1;
	}

	if (testtype == USB_TYPE_ISOCHRONOUS) {
		ret = test_isoc_sync(devh);
	} else if (testtype == USB_TYPE_BULK) {
		ret = test_bulk_sync(devh);
	} else if (testtype == USB_TYPE_INTERRUPT) {
		ret = test_intr_sync(devh, testloop);
	}

	if (ret != 0) {
		printf("TEST SYNC XFER FAIL\n");
		return -1;
	}

	return 0;
}

int test_async_xfer(libusb_dev_handle_t devh)
{
	int ret;

	ret = async_xfer_ctrl_test(devh);
	if (ret != 0) {
		printf("ASYNC CTRL xfer fail\n");
		return -1;
	}
	
	ret = async_xfer_test(devh, testtype, testloop);
	if (ret != 0) {
		printf("TEST ASYNC XFER FAIL\n");
		return -1;
	}
	
	return 0;
}

int advance_xfer_test(void)
{
	int ret = 0;
	libusb_dev_handle_t devh;
	libusb_devid_t devid;
	libusb_handle_t libh;
	uint8_t cfg;
	uint8_t alt;

	/* ret = libusb_get_devids_by_vendor(libhandle,0x4b4,0x8613,
	   	&devids,&devnum); 
	
		printf("libusb_get_devids_by_vendor: PASS\n");
	*/

	ret = libusb_get_devids_by_class(libhandle, 0xff,
		-1, -1, &devids, &devnum);
	if(ret < 0) {
		printf("Error get devids by class:%s\n",libusb_strerror(ret));
		return -1;
	}

	ret = libusb_open_device(libhandle,devids[0],0,&devh);
	if (ret != 0) {
		printf("Open device(%d) error:%s\n",(int)devids[0],
				libusb_strerror(ret));
		goto err;
	}
	printf("Device(%llu) opened: %llu\n",devids[0],devh);

	ret = libusb_get_devid(devh, &devid);
	if(ret < 0) {
		printf("Error get devids by handle:%s\n",libusb_strerror(ret));
		goto err;
	}
	printf("devh=%x devid=%x\n",(int)devh,(int)devid);

	ret = libusb_get_lib_handle(devh,&libh);
	if (ret != 0) {
		printf("Get device(%llu) lib handle error:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}
	printf("Lib handle = %llu\n",libh);

#if 0  /* multi xfer test */
	ret = multi_xfer_test(devh);
	if(ret !=0) {
		libusb_free_devid_list(devids);
		printf("multi_xfer_test error:%s\n", libusb_strerror(ret));
		return -1;

	}
#endif
	
	if (testsync == 1) {
		ret = test_sync_xfer(devh);
		if (ret != 0) {
			goto err;
		}
	}


	if (testasync == 1) {
		ret = test_async_xfer(devh);
		if (ret != 0) {
			goto err;
		}
	}


#if 1	/* configuration test */
	ret = libusb_set_configuration(devh,1);
	if (ret != 0) {
		printf("Set device(%llu) config error:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}

	ret = libusb_get_configuration(devh,&cfg);
	if (ret != 0) {
		printf("Get device(%llu) config error:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}
	printf("Configuration= %d\n",cfg);

#endif

#if 1	/* claim interface test */
	ret = libusb_claim_interface(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) claim interface error:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}

	ret = libusb_is_interface_claimed(devh,0);
	printf("Device(%llu) interface(0) claimed return %d\n", devids[0],
			ret);

	ret = libusb_set_altsetting(devh,0,0);
	if (ret != 0) {
		printf("Device(%llu) interface(0) set alt:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}


	ret = libusb_get_altsetting(devh,0,&alt);
	if (ret != 0) {
		printf("Device(%llu) interface(0) get alt:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}
	printf("Interface(0) alt=%d\n",alt);

	ret = libusb_release_interface(devh,0);
	if (ret != 0) {
		printf("Device(%llu) release interface error:%s\n",devids[0],
				libusb_strerror(ret));
		goto err;
	}
	/*
	   ret = libusb_reset(devh);
	   if (ret != 0) {
	   libusb_free_devid_list(devids);
	   printf("Device(%llu) reset error:%s\n",devids[0],
	   libusb_strerror(ret));
	   return -1;
	   }
	 */

#endif

err:
	libusb_close_device(devh);
	libusb_free_devid_list(devids);
	return ret;
}

void usage(char *prog)
{
	printf("usage:\n");
	printf("%s\n", prog);
	printf("\tBasic API test\n");

	printf("OR Advanced Xfer Test\n");

	printf("%s [-t <intr|isoc|bulk|ctrl>] [-l] [-m] [-a] [-s]\n", prog);
	printf("Where:\n");
	printf("\t-t transfer type\n"
		"\t-l loop test\n"
		"\t-m multi request test\n"
		"\t-a async xfer test\n"
		"\t-s sync xfer test\n");
}


int parse_option(int argc, char *argv[])
{
	char c;
	while((c = getopt(argc, argv, "t:lmas")) != -1) {
		switch (c) {
			case 't':
				if (strcmp(optarg, "ctrl") == 0) {
					testtype = USB_TYPE_CONTROL;
				} else if (strcmp(optarg, "bulk") == 0) {
					testtype = USB_TYPE_BULK;
				} else if (strcmp(optarg, "intr") == 0) {
					testtype = USB_TYPE_INTERRUPT;
				} else if (strcmp(optarg, "isoc") == 0) {
					testtype = USB_TYPE_ISOCHRONOUS;
				} else {
					printf("Unknown type\n");
					goto err1;
				}
				break;

			case 'l':
				testloop = LOOP;
				break;
			case 'm':
				testmulti = 1;
				break;
			case 'a':
				testasync = 1;
				break;
			case 's':
				testsync = 1;
				break;
			case ':':
				printf("-%c has no arguments\n",optopt);
				goto err1;
			case '?':
			default:
				printf("Unknown option: -%c\n",optopt);
				goto err1;
		}
	}
			
	return 0;
err1:
	usage(argv[0]);
	return -1;
}


/*
 * initialize libusb, set global timeout, callbacks
 *
 * Get all buses on the system.
 * Get every device's data and print them. 
 */
int basic_test(void)
{
	int ret;
	uint32_t flags = 0;

	ret = libusb_init(flags,&libhandle);
	if(ret < 0) {
		printf("error init\n");
		exit(1);
	}
	printf("lib handle=%llu \n",libhandle);
	printf("libusb_init PASS\n");

	ret = libusb_set_default_timeout(libhandle,USB_TYPE_CONTROL,10);
	if(ret) {
		printf("set timeout error: %s\n",libusb_strerror(ret));
		return -1;
	}

	printf("libusb_set_default_timeout : PASS\n");

	ret = libusb_set_event_callback(libhandle,USB_REMOVE,event_cb,NULL);
	if(ret) {
		printf("set event callback error: %s\n",libusb_strerror(ret));
		return -1;
	}
	printf("libusb_set_event_callback: PASS\n");

	/*get buses */
	ret = libusb_get_busid_list(libhandle,&bus,&busnum);
	if(ret) {
		printf("busid error: %s\n",libusb_strerror(ret));
		return -1;
	}
	printf("libusb_get_busid_list: PASS\n");
	
	test_get_device_data();

	return 0;
}

void cleanup(void)
{
	libusb_free_busid_list(bus);

	libusb_fini(libhandle);
	printf("libusb_fini PASS\n");

}

int main(int argc, char *argv[])
{
	int ret;

	if(parse_option(argc, argv) < 0) {
		exit(1);
	}
	
	ret = basic_test();
	if (ret < 0) {
		exit(1);
	}

	advance_xfer_test();

	cleanup();	

	exit(0);
}
