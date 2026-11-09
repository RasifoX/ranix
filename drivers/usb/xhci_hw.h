/* xHCI registers, TRB types and the driver's limits (xHCI spec 1.2, ch. 5-6).
 * Included by drivers/usb/xhci.c only. */
#ifndef DRIVERS_USB_XHCI_HW_H
#define DRIVERS_USB_XHCI_HW_H

/* capability registers */
#define CAP_CAPLENGTH   0x00   /* byte 0; HCIVERSION at byte 2 */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* operational registers (relative to op base) */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTS        0x400   /* PORTSC[i] at 0x400 + i*0x10 */

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)  /* controller not ready */

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_OCA      (1u << 3)   /* over-current active */
#define PORTSC_OCC      (1u << 20)  /* over-current change (write 1 to clear) */
#define PORTSC_CSC      (1u << 17)  /* connect status change */
#define PORTSC_CHANGES  (0x7Fu << 17)   /* every write-1-to-clear change bit */
/* What a write to PORTSC may carry back. PED and the change bits are write-1-to-clear,
 * so echoing a read would disable the port. */
#define PORTSC_KEEP     (PORTSC_PP | (3u << 14) | (7u << 25))

/* runtime / interrupter 0 (relative to rt base) */
#define RT_IR0          0x20
#define IR_IMAN         0x00
#define IR_IMOD         0x04
#define IR_ERSTSZ       0x08
#define IR_ERSTBA       0x10
#define IR_ERDP         0x18

/* TRB types */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_RESET_EP        14
#define TRB_EVALUATE_CTX    13
#define TRB_STOP_EP         15
#define TRB_SET_TR_DEQ      16
#define TRB_EVT_TRANSFER    32
#define TRB_EVT_CMD_COMP    33
#define TRB_EVT_PORT_CHANGE 34

#define TRB_C     (1u << 0)        /* cycle bit */
#define TRB_ENT   (1u << 1)
#define TRB_ISP   (1u << 2)
#define TRB_CH    (1u << 4)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_TYPE(t)  ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3F)
#define TRB_GET_CC(s)    (((s) >> 24) & 0xFF)   /* completion code in status */
#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13

/* Timeouts in milliseconds; a bridge can take seconds on its first command. */
#define CMD_MS    2000u
#define XFER_MS   15000u
#define CTRL_MS   3000u

typedef struct {
	uint32_t lo, hi, status, control;
} trb_t;

#define RING_TRBS  256
#define EVT_TRBS   256
#define MAX_DISKS  16           /* names usb0..: a pulled disk's name is reused once nothing holds it */
#define MAX_DEVS   24
#define MAX_HID    3            /* interfaces per device */
#define MAX_PORTS  64
#define DATA_ORDER 5
#define DATA_BYTES (4096u << DATA_ORDER)

#endif
