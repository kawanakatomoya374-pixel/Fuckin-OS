/**
 * usb.h - USB Host Controller Driver
 */

#ifndef USB_H
#define USB_H

#include "types.h"

/* USB constants */
#define USB_MAX_DEVICES     127
#define USB_MAX_ENDPOINTS   16
#define USB_CTRL_TIMEOUT    5000

/* USB speeds */
#define USB_SPEED_LOW       0
#define USB_SPEED_FULL      1
#define USB_SPEED_HIGH      2
#define USB_SPEED_SUPER     3

/* USB request types */
#define USB_REQ_HOST_TO_DEVICE  0x00
#define USB_REQ_DEVICE_TO_HOST  0x80
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_RECIPIENT_DEVICE    0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT  0x02

/* USB standard requests */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIG      0x08
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_GET_INTERFACE   0x0A
#define USB_REQ_SET_INTERFACE   0x0B
#define USB_REQ_SYNCH_FRAME     0x0C

/* USB descriptors */
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

/* USB device structure */
typedef struct usb_device {
    uint8_t  address;
    uint8_t  speed;
    uint8_t  max_packet0;
    uint64_t vendor;
    uint64_t product;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  protocol;
    
    struct usb_device* parent;
    struct usb_device* next;
} usb_dev_t;

/* USB request structure */
typedef struct usb_request {
    uint8_t  type;
    uint8_t  request;
    uint64_t value;
    uint64_t index;
    uint64_t length;
    void*    data;
} usb_req_t;

/* Function prototypes */
void usb_init(void);
bool usb_is_initialized(void);
bool usb_has_usb2(void);
const char* usb_get_last_error(void);
void usb_get_status(char* out, size_t out_size);
int usb_control_transfer(usb_dev_t* dev, usb_req_t* req);
int usb_bulk_transfer(usb_dev_t* dev, int endpoint, void* data, int len);
int usb_interrupt_transfer(usb_dev_t* dev, int endpoint, void* data, int len);

#endif /* USB_H */
