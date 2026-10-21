/******************************************************************************
 * @file     usb_hid.h
 * @brief    Native Windows HID transport for the 64-byte reports used by the
 *           Nuvoton M3331 HID ISP channel. Uses the OS HID driver, so no
 *           WinUSB/Zadig driver replacement is required.
 ******************************************************************************/
#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

#define HID_PACKET_SIZE 64

/* Receive result codes (also used as generic error return where noted). */
#define HID_RECV_TIMEOUT 0
#define HID_RECV_ERROR   (-1)

/**
 * Open the first HID device matching vid/pid.
 * Returns 0 on success, non-zero on failure.
 */
int hid_open(uint16_t vid, uint16_t pid);

/**
 * Select the HID report IDs used by the transport.
 *   out_id : report ID prepended to every OUT report (0 = un-numbered).
 *   in_id  : report ID expected on IN reports (0 = accept any).
 * The CFU application's ISP channel uses 40 (OUT) / 41 (IN).
 */
void hid_set_report_ids(uint8_t out_id, uint8_t in_id);

/** Close the device handle. */
void hid_close(void);

/**
 * Send an output report carrying exactly HID_PACKET_SIZE data bytes.
 * Returns 0 on success, non-zero on failure.
 */
int hid_send(const uint8_t *buf, int timeout_ms);

/**
 * Receive an input report and copy its HID_PACKET_SIZE data bytes into buf.
 * Returns the number of data bytes (>0), HID_RECV_TIMEOUT on timeout,
 * or HID_RECV_ERROR on any other error.
 */
int hid_recv(uint8_t *buf, int timeout_ms);

#endif /* USB_HID_H */
