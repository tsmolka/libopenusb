#ifndef EZUSB_H
#define EZUSB_H

int ezusb_load_image(char *filename);
int ezusb_write_image(libusb_dev_handle_t dev);

#endif /* EZUSB_H */

