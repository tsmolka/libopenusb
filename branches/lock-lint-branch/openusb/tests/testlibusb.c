/*
 * testlibusb.c
 *
 * Test suite. It uses Keyspan Serial devices which have a customizable
 * Cypress (formerly Anchor Chips) AN2131SC. We then load a new firmware
 * onto the device which allows us to test various features of libusb
 * (and indirectly the kernel)
 */

#include <stdio.h>
#include <pthread.h>

#include <libusb.h>

#include "ezusb.h"

struct test_device {
  libusb_device_id_t devid;	/* Device ID */
  libusb_dev_handle_t dev;	/* Opened handle */
  pthread_t thread;		/* Thread used for testing */
};

int num_test_devices = 0;
struct test_device test_devices[8];

#define FIRMWARE "usbtest_fw.ihx"

#define BULK_OUT_EP	(USB_ENDPOINT_OUT | 2)
#define BULK_IN_EP	(USB_ENDPOINT_IN | 2)

void *run_test(void *_test)
{
  struct test_device *test = _test;
  char buf[1024] = { 0 };
  struct libusb_bulk_request bulkout = {
    .dev = test->dev,
    .endpoint = BULK_OUT_EP,
    .buf = buf,
    .buflen = sizeof(buf),
    .timeout = 1000,
  }, bulkin = {
    .dev = test->dev,
    .endpoint = BULK_IN_EP,
    .buf = buf,
    .buflen = sizeof(buf),
    .timeout = 1000,
  };
  int ret;

  while (1) {
    size_t transferred_bytes;

printf("out\n");
    ret = libusb_bulk(&bulkout, &transferred_bytes);
    if (ret < 0)
      fprintf(stderr, "write failed (ret = %d)\n", ret);

printf("in\n");
    ret = libusb_bulk(&bulkin, &transferred_bytes);
    if (ret < 0)
      fprintf(stderr, "read failed (ret = %d)\n", ret);
  }

  return NULL;
}

int new_keyspan_device(libusb_device_id_t devid)
{
  libusb_dev_handle_t dev;
  int ret;

  printf("Loading test firmware onto device %d\n", devid);

  ret = libusb_open(devid, &dev);
  if (ret) {
    fprintf(stderr, "unable to open device (ret = %d)\n", ret);
    return 1;
  }

  ret = libusb_claim_interface(dev, 0);
  if (ret) {
    ret = libusb_detach_kernel_driver_np(dev, 0);
    if (ret)
      fprintf(stderr, "unable to release already claimed interface (ret = %d)\n", ret);

    ret = libusb_claim_interface(dev, 0);
    if (ret) {
      fprintf(stderr, "unable to claim interface (ret = %d)\n", ret);
      return 1;
    }
  }

  ret = ezusb_write_image(dev);
  if (ret) {
    fprintf(stderr, "unable to load image into device memory (ret = %d)\n", ret);
    return 1;
  }

  libusb_close(dev);

  return 0;
}

int new_test_device(libusb_device_id_t devid)
{
  struct test_device *test;
  libusb_dev_handle_t dev;
  int ret;

  printf("Found new usbtest device %d\n", devid);

  ret = libusb_open(devid, &dev);
  if (ret) {
    fprintf(stderr, "unable to open device (ret = %d)\n", ret);
    return 1;
  }

  ret = libusb_claim_interface(dev, 0);
  if (ret) {
    ret = libusb_detach_kernel_driver_np(dev, 0);
    if (ret)
      fprintf(stderr, "unable to release already claimed interface (ret = %d)\n", ret);

    ret = libusb_claim_interface(dev, 0);
    if (ret) {
      fprintf(stderr, "unable to claim interface (ret = %d)\n", ret);
      return 1;
    }
  }

  /* FIXME: Add some locking here and make sure we cleanup on error */
  test = &test_devices[num_test_devices++];

  test->devid = devid;
  test->dev = dev;

  /* Select alt setting 1 to activate the endpoints we need */
  ret = libusb_set_altinterface(dev, 1);
  if (ret < 0) {
    fprintf(stderr, "unable to set alternate interface (ret = %d)\n", ret);
    return 1;
  }

  /* Spawn thread */
  if (pthread_create(&test->thread, NULL, run_test, test)) {
    fprintf(stderr, "unable to start test thread\n");
    return 1;
  }

  return 0;
}

void attach_callback(libusb_device_id_t devid,
	enum libusb_event_type event_type, void *arg)
{
  struct usb_device_desc desc;
  int ret;

  ret = libusb_get_device_desc(devid, &desc);
  if (ret < 0) {
    printf("error retrieving device descriptor (ret = %d)\n", ret);
    return;
  }

  printf("new device %x %x\n", desc.idVendor, desc.idProduct);

  if (desc.idVendor == 0x06cd && desc.idProduct == 0x010c)
    new_keyspan_device(devid);

  if (desc.idVendor == 0xfff0 && desc.idProduct == 0xfff0)
    new_test_device(devid);
}

void detach_callback(libusb_device_id_t devid,
	enum libusb_event_type event_type, void *arg)
{
  int i;

  for (i = 0; i < num_test_devices; i++) {
    if (test_devices[i].devid == devid)
      printf("Removing usbtest device %d\n", devid);
  }
}

int main(void)
{
  libusb_device_id_t devid;
  libusb_match_handle_t match;
  int ret, prev_num;

  libusb_init();

  libusb_set_event_callback(USB_ATTACH, attach_callback, NULL);
  libusb_set_event_callback(USB_DETACH, detach_callback, NULL);

  if (ezusb_load_image(FIRMWARE) && ezusb_load_image("tests/" FIRMWARE)) {
    fprintf(stderr, "unable to load firmware image %s\n", FIRMWARE);
    return 1;
  }

  /* Find Keyspan devices for us to "hijack" */
  ret = libusb_match_devices_by_vendor(&match, 0x06cd, 0x010c);
  if (ret < 0) {
    fprintf(stderr, "unable to create match (ret = %d)\n", ret);
    return ret;
  }

  while (libusb_match_next_device(match, &devid) == 0)
    ret = new_keyspan_device(devid);

  libusb_match_terminate(match);

  /* Find the devices running the test firmware for us to use */
  ret = libusb_match_devices_by_vendor(&match, 0xfff0, 0xfff0);
  if (ret < 0) {
    fprintf(stderr, "unable to create match (ret = %d)\n", ret);
    return ret;
  }

  while (libusb_match_next_device(match, &devid) == 0)
    new_test_device(devid);

  libusb_match_terminate(match);

  prev_num = 0;
  while (1) {
    if (prev_num != num_test_devices) {
      printf("%d devices\n", num_test_devices);
      prev_num = num_test_devices;
    }

    sleep(1);
  }

  return 0;
}

