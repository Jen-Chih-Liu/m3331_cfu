/******************************************************************************
 * @file     isp_proto.c
 * @brief    Nuvoton M3331 HID ISP command layer (host side).
 ******************************************************************************/
#include "isp_proto.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "usb_hid.h"

/* Command-response timeouts (milliseconds). */
#define ACK_TIMEOUT_MS   1000
#define ERASE_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS  2000
#define RECV_POLL_MS     200

/* Payload byte counts per packet. */
#define FIRST_DATA_BYTES 48  /* data carried by the initial UPDATE_APROM packet */
#define CHUNK_DATA_BYTES 56  /* data carried by each subsequent packet          */

static uint32_t s_packno = 1;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Elapsed milliseconds since `start` (clock() ticks are milliseconds on MSVC). */
static long elapsed_ms(clock_t start)
{
    return (long)((clock() - start) * 1000 / CLOCKS_PER_SEC);
}

/*
 * Wait for the acknowledgement whose packet number equals s_packno + 1.
 * On success advances s_packno by 2 and copies the response into `resp`.
 */
static int wait_ack(uint8_t *resp, int timeout_ms)
{
    clock_t start = clock();

    while (elapsed_ms(start) < timeout_ms)
    {
        int n = hid_recv(resp, RECV_POLL_MS);

        if (n > 0)
        {
            if (get_u32(resp + 4) == s_packno + 1)
            {
                s_packno += 2;
                return ISP_OK;
            }
        }
        else if (n == HID_RECV_ERROR)
        {
            return ISP_ERR_IO;
        }
    }

    return ISP_ERR_TMOUT;
}

/*
 * Send a single-packet command whose payload (bytes 8..63) is `payload`,
 * then wait for the acknowledgement. The response is returned in `resp`.
 */
static int cmd_simple(uint32_t cmd, const uint8_t *payload, int payload_len,
                      uint8_t *resp, int ack_timeout_ms)
{
    uint8_t req[HID_PACKET_SIZE];

    memset(req, 0, sizeof(req));
    put_u32(req + 0, cmd);
    put_u32(req + 4, s_packno);

    if (payload != NULL && payload_len > 0)
    {
        if (payload_len > HID_PACKET_SIZE - 8)
            payload_len = HID_PACKET_SIZE - 8;
        memcpy(req + 8, payload, (size_t)payload_len);
    }

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return ISP_ERR_IO;

    return wait_ack(resp, ack_timeout_ms);
}

void isp_reset_packno(void)
{
    s_packno = 1;
}

int isp_connect(void)
{
    uint8_t resp[HID_PACKET_SIZE];
    return cmd_simple(CMD_CONNECT, NULL, 0, resp, ACK_TIMEOUT_MS);
}

int isp_sync_packno(void)
{
    uint8_t resp[HID_PACKET_SIZE];
    uint8_t payload[4];

    /* SYNC_PACKNO carries the desired packet number in the payload. */
    put_u32(payload, s_packno);
    return cmd_simple(CMD_SYNC_PACKNO, payload, sizeof(payload), resp, ACK_TIMEOUT_MS);
}

int isp_get_deviceid(uint32_t *pdid)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_simple(CMD_GET_DEVICEID, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == ISP_OK && pdid != NULL)
        *pdid = get_u32(resp + 8);

    return r;
}

int isp_get_fwver(uint8_t *ver)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_simple(CMD_GET_FWVER, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == ISP_OK && ver != NULL)
        *ver = resp[8];

    return r;
}

int isp_get_fwver32(uint32_t *ver)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_simple(CMD_GET_FWVER, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == ISP_OK && ver != NULL)
        *ver = get_u32(resp + 8);

    return r;
}

int isp_read_config(uint32_t *cfg0, uint32_t *cfg1)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_simple(CMD_READ_CONFIG, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == ISP_OK)
    {
        if (cfg0 != NULL)
            *cfg0 = get_u32(resp + 8);
        if (cfg1 != NULL)
            *cfg1 = get_u32(resp + 12);
    }

    return r;
}

int isp_get_flashmode(uint32_t *mode)
{
    uint8_t resp[HID_PACKET_SIZE];
    int r = cmd_simple(CMD_GET_FLASHMODE, NULL, 0, resp, ACK_TIMEOUT_MS);

    if (r == ISP_OK && mode != NULL)
        *mode = get_u32(resp + 8);

    return r;
}

int isp_update_aprom(const uint8_t *data, uint32_t size,
                     isp_progress_cb progress, uint16_t *out_checksum)
{
    uint8_t req[HID_PACKET_SIZE];
    uint8_t resp[HID_PACKET_SIZE];
    uint32_t first_len;
    uint32_t offset;
    int last_percent = -1;
    int r;

    if (data == NULL || size == 0)
        return ISP_ERR_IO;

    /* --- First packet: command + address + length + first data chunk. --- */
    memset(req, 0, sizeof(req));
    put_u32(req + 0, CMD_UPDATE_APROM);
    put_u32(req + 4, s_packno);
    put_u32(req + 8, 0);       /* start address (APROM base) */
    put_u32(req + 12, size);   /* total length to program    */

    first_len = (size < FIRST_DATA_BYTES) ? size : FIRST_DATA_BYTES;
    memcpy(req + 16, data, (size_t)first_len);

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return ISP_ERR_IO;

    /* The device erases APROM before acknowledging, so allow extra time. */
    r = wait_ack(resp, ERASE_TIMEOUT_MS);
    if (r != ISP_OK)
        return r;

    if (progress != NULL)
    {
        progress(0);
        last_percent = 0;
    }

    /* --- Remaining data packets: 56 data bytes each at offset 8. --- */
    for (offset = FIRST_DATA_BYTES; offset < size; offset += CHUNK_DATA_BYTES)
    {
        uint32_t chunk = size - offset;
        if (chunk > CHUNK_DATA_BYTES)
            chunk = CHUNK_DATA_BYTES;

        memset(req, 0, sizeof(req));
        put_u32(req + 4, s_packno);
        memcpy(req + 8, data + offset, (size_t)chunk);

        if (hid_send(req, SEND_TIMEOUT_MS) != 0)
            return ISP_ERR_IO;

        r = wait_ack(resp, ACK_TIMEOUT_MS);
        if (r != ISP_OK)
            return r;

        if (progress != NULL)
        {
            int percent = (int)(((uint64_t)(offset + chunk) * 100) / size);
            if (percent != last_percent)
            {
                progress(percent);
                last_percent = percent;
            }
        }
    }

    /* The final acknowledgement carries the device flash checksum. */
    if (out_checksum != NULL)
        *out_checksum = (uint16_t)(resp[8] | (resp[9] << 8));

    if (progress != NULL && last_percent != 100)
        progress(100);

    return ISP_OK;
}

int isp_read_aprom(uint32_t addr, uint8_t *data, uint32_t size,
                   isp_progress_cb progress)
{
    uint8_t resp[HID_PACKET_SIZE];
    uint8_t payload[4];
    uint32_t offset;
    int last_percent = -1;
    int r;

    if (data == NULL || size == 0)
        return ISP_ERR_IO;

    for (offset = 0; offset < size; offset += READ_APROM_CHUNK)
    {
        uint32_t chunk = size - offset;
        if (chunk > READ_APROM_CHUNK)
            chunk = READ_APROM_CHUNK;

        put_u32(payload, addr + offset);
        r = cmd_simple(CMD_READ_APROM, payload, sizeof(payload), resp,
                       ACK_TIMEOUT_MS);
        if (r != ISP_OK)
            return r;

        /* Response data area begins at byte 8. */
        memcpy(data + offset, resp + 8, (size_t)chunk);

        if (progress != NULL)
        {
            int percent = (int)(((uint64_t)(offset + chunk) * 100) / size);
            if (percent != last_percent)
            {
                progress(percent);
                last_percent = percent;
            }
        }
    }

    if (progress != NULL && last_percent != 100)
        progress(100);

    return ISP_OK;
}

int isp_run_aprom(void)
{
    uint8_t req[HID_PACKET_SIZE];

    /* The device resets immediately, so no acknowledgement is expected. */
    memset(req, 0, sizeof(req));
    put_u32(req + 0, CMD_RUN_APROM);
    put_u32(req + 4, s_packno);

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return ISP_ERR_IO;

    s_packno += 2;
    return ISP_OK;
}

int isp_run_ldrom(void)
{
    uint8_t req[HID_PACKET_SIZE];

    /* The device jumps into the LDROM bootloader, so no ACK is expected. */
    memset(req, 0, sizeof(req));
    put_u32(req + 0, CMD_RUN_LDROM);
    put_u32(req + 4, s_packno);

    if (hid_send(req, SEND_TIMEOUT_MS) != 0)
        return ISP_ERR_IO;

    s_packno += 2;
    return ISP_OK;
}

uint16_t isp_calc_checksum(const uint8_t *data, uint32_t size)
{
    uint16_t sum = 0;
    uint32_t i;

    for (i = 0; i < size; i++)
        sum = (uint16_t)(sum + data[i]);

    return sum;
}
