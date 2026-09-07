/******************************************************************************
 * @file     main.c
 * @brief    HID_ISP_TOOL - host programmer for the Nuvoton M3331 HID ISP
 *           bootloader (ISP_HID_20 firmware).
 *
 * Usage:
 *   HID_ISP_TOOL <firmware.bin>   Program APROM, verify checksum, run APROM.
 *   HID_ISP_TOOL --info           Read device ID / FW version / config only.
 *   HID_ISP_TOOL <fw.bin> --no-run  Program and verify but stay in bootloader.
 *   HID_ISP_TOOL --dump <file>    Read back the 256 KB APROM into a .bin file.
 *   HID_ISP_TOOL --run-ldrom      Switch the CFU application into the LDROM
 *                                 ISP bootloader (then re-run to program).
 *   HID_ISP_TOOL --help           Show usage.
 *
 * Two firmware targets speak the HID ISP protocol and are auto-detected:
 *   LDROM ISP bootloader (ISP_HID_20)          : VID 0x0416 / PID 0x3F00.
 *   CFU application ISP channel (HSUSBD_HID_CFU): VID 0x0416 / PID 0xF502,
 *                                                 HID report IDs 40 / 41.
 * APROM programming is only available on the LDROM bootloader; the CFU
 * channel is used for info queries and to hand control to the bootloader.
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "isp_proto.h"
#include "usb_hid.h"

/* APROM size of the M3331 target (used as an upper bound for the image). */
#define MAX_APROM_SIZE (256 * 1024)

/* Full APROM size read back by --dump (AP0 + AP1 + Calibration + OEM). */
#define APROM_DUMP_SIZE (256 * 1024)

static void print_usage(const char *prog)
{
    printf("HID ISP host programmer for Nuvoton M3331 (ISP_HID_20 / HSUSBD_HID_CFU)\n\n");
    printf("Usage:\n");
    printf("  %s <firmware.bin>          Program APROM, verify, then run APROM\n", prog);
    printf("  %s <firmware.bin> --no-run Program and verify, stay in bootloader\n", prog);
    printf("  %s --info                  Read device info only (no programming)\n", prog);
    printf("  %s --dump <file>           Read back the 256 KB APROM into <file>\n", prog);
    printf("  %s --run-ldrom             Switch CFU app into the LDROM bootloader\n", prog);
    printf("  %s --help                  Show this help\n", prog);
    printf("\nOptions:\n");
    printf("  --ldrom   Force the LDROM bootloader channel (PID 0x3F00)\n");
    printf("  --cfu     Force the CFU application channel (PID 0xF502, report 40/41)\n");
}

static void progress_cb(int percent)
{
    printf("\rProgramming: %3d%%", percent);
    fflush(stdout);
}

/* Load a binary file into a freshly allocated buffer. Caller frees *out_buf. */
static int load_bin(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
    FILE *fp;
    long size;
    uint8_t *buf;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        fprintf(stderr, "Cannot open file: %s\n", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0)
    {
        fprintf(stderr, "File is empty: %s\n", path);
        fclose(fp);
        return -1;
    }

    if (size > MAX_APROM_SIZE)
    {
        fprintf(stderr, "File too large (%ld bytes, max %d)\n", size, MAX_APROM_SIZE);
        fclose(fp);
        return -1;
    }

    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        fclose(fp);
        return -1;
    }

    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        fprintf(stderr, "Failed to read file: %s\n", path);
        free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_size = (uint32_t)size;
    return 0;
}

/* Write `size` bytes from `buf` to `path`. Returns 0 on success. */
static int save_bin(const char *path, const uint8_t *buf, uint32_t size)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL)
    {
        fprintf(stderr, "Cannot create file: %s\n", path);
        return -1;
    }

    if (fwrite(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        fprintf(stderr, "Failed to write file: %s\n", path);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Open the ISP device and configure the transport report IDs.
 *   pref: 0 = auto (LDROM then CFU), 1 = force LDROM, 2 = force CFU.
 * On success sets *is_cfu (1 = CFU application channel, 0 = LDROM bootloader).
 */
static int open_target(int pref, int *is_cfu)
{
    if (pref != 2) /* auto or forced LDROM */
    {
        hid_set_report_ids(0x00, 0x00);
        if (hid_open(ISP_USB_VID, ISP_USB_PID_LDROM) == 0)
        {
            *is_cfu = 0;
            return 0;
        }
        if (pref == 1)
            return -1;
        printf("LDROM bootloader (0x3F00) not present, trying CFU app (0xF502)...\n");
    }

    hid_set_report_ids(ISP_REPORT_ID_OUT, ISP_REPORT_ID_IN);
    if (hid_open(ISP_USB_VID, ISP_USB_PID_CFU) == 0)
    {
        *is_cfu = 1;
        return 0;
    }
    return -1;
}

/* Establish a session and print the device identification block. */
static int connect_and_report(int is_cfu)
{
    uint32_t pdid = 0, cfg0 = 0, cfg1 = 0, mode = 0;

    isp_reset_packno();

    if (isp_connect() != ISP_OK)
    {
        fprintf(stderr, "CONNECT failed\n");
        return -1;
    }
    if (isp_sync_packno() != ISP_OK)
    {
        fprintf(stderr, "SYNC_PACKNO failed\n");
        return -1;
    }

    printf("Channel   : %s\n",
           is_cfu ? "CFU application (PID 0xF502, report 40/41)"
                  : "LDROM bootloader (PID 0x3F00)");

    if (isp_get_deviceid(&pdid) == ISP_OK)
        printf("Device ID : 0x%08X\n", pdid);

    if (is_cfu)
    {
        uint32_t ver = 0;
        if (isp_get_fwver32(&ver) == ISP_OK)
            printf("APROM ver : 0x%08X\n", ver);
    }
    else
    {
        uint8_t ver = 0;
        if (isp_get_fwver(&ver) == ISP_OK)
            printf("ISP FW ver: 0x%02X\n", ver);
    }

    if (isp_read_config(&cfg0, &cfg1) == ISP_OK)
        printf("CONFIG0   : 0x%08X\nCONFIG1   : 0x%08X\n", cfg0, cfg1);
    if (isp_get_flashmode(&mode) == ISP_OK)
        printf("Boot mode : %s\n", (mode == 2) ? "LDROM" : "APROM");

    return 0;
}

int main(int argc, char *argv[])
{
    const char *firmware = NULL;
    int info_only = 0;
    int no_run = 0;
    int run_ldrom = 0;
    const char *dump_path = NULL;
    int pref = 0;          /* 0 = auto, 1 = force LDROM, 2 = force CFU */
    int is_cfu = 0;
    int i;
    uint8_t *image = NULL;
    uint32_t image_size = 0;
    uint16_t dev_cksum = 0, host_cksum = 0;
    time_t t_start, t_end;
    int rc = 0;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--info") == 0 || strcmp(argv[i], "-i") == 0)
        {
            info_only = 1;
        }
        else if (strcmp(argv[i], "--no-run") == 0)
        {
            no_run = 1;
        }
        else if (strcmp(argv[i], "--run-ldrom") == 0)
        {
            run_ldrom = 1;
        }
        else if (strcmp(argv[i], "--dump") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "--dump requires an output file path\n");
                return 1;
            }
            dump_path = argv[++i];
        }
        else if (strcmp(argv[i], "--ldrom") == 0)
        {
            pref = 1;
        }
        else if (strcmp(argv[i], "--cfu") == 0)
        {
            pref = 2;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        else
        {
            firmware = argv[i];
        }
    }

    if (!info_only && !run_ldrom && dump_path == NULL && firmware == NULL)
    {
        print_usage(argv[0]);
        return 1;
    }

    /* Load the firmware image before touching USB so we fail fast on bad input. */
    if (!info_only && !run_ldrom && dump_path == NULL)
    {
        if (load_bin(firmware, &image, &image_size) != 0)
            return 1;
    }

    if (open_target(pref, &is_cfu) != 0)
    {
        fprintf(stderr, "Cannot open ISP device. Is it in bootloader or CFU mode?\n");
        free(image);
        return 1;
    }

    t_start = time(NULL);

    if (connect_and_report(is_cfu) != 0)
    {
        rc = 1;
        goto done;
    }

    /* Hand control to the LDROM bootloader (CFU application only). */
    if (run_ldrom)
    {
        if (!is_cfu)
        {
            printf("Already on the LDROM bootloader channel; nothing to do.\n");
            goto done;
        }
        if (isp_run_ldrom() != ISP_OK)
        {
            fprintf(stderr, "RUN_LDROM command failed\n");
            rc = 1;
            goto done;
        }
        printf("Switching to LDROM bootloader. Re-run to program APROM.\n");
        goto done;
    }

    /* Read back the whole APROM into a .bin file. */
    if (dump_path != NULL)
    {
        uint8_t *dump = (uint8_t *)malloc(APROM_DUMP_SIZE);
        if (dump == NULL)
        {
            fprintf(stderr, "Out of memory\n");
            rc = 1;
            goto done;
        }
        if (is_cfu)
        {
            fprintf(stderr,
                    "Device is running the CFU application (PID 0xF502); its ISP "
                    "channel cannot read APROM.\n"
                    "Run '%s --run-ldrom' to switch into the LDROM bootloader, "
                    "then re-run --dump.\n", argv[0]);
            free(dump);
            rc = 1;
            goto done;
        }
        printf("Dumping   : %s (%d bytes)\n", dump_path, APROM_DUMP_SIZE);
        if (isp_read_aprom(0, dump, APROM_DUMP_SIZE, progress_cb) != ISP_OK)
        {
            fprintf(stderr, "\nAPROM read failed\n");
            free(dump);
            rc = 1;
            goto done;
        }
        printf("\n");
        if (save_bin(dump_path, dump, APROM_DUMP_SIZE) != 0)
        {
            free(dump);
            rc = 1;
            goto done;
        }
        printf("APROM saved to %s\n", dump_path);
        free(dump);
        goto done;
    }

    if (info_only)
        goto done;

    /* The CFU application ISP channel cannot erase/program APROM. */
    if (is_cfu)
    {
        fprintf(stderr,
                "Device is running the CFU application (PID 0xF502); its ISP "
                "channel cannot program APROM.\n"
                "Run '%s --run-ldrom' to switch into the LDROM bootloader, then "
                "re-run to program.\n", argv[0]);
        rc = 1;
        goto done;
    }

    printf("Firmware  : %s (%u bytes)\n", firmware, image_size);

    if (isp_update_aprom(image, image_size, progress_cb, &dev_cksum) != ISP_OK)
    {
        fprintf(stderr, "\nAPROM programming failed\n");
        rc = 1;
        goto done;
    }
    printf("\n");

    host_cksum = isp_calc_checksum(image, image_size);
    if (dev_cksum == host_cksum)
    {
        printf("Checksum  : 0x%04X (verified)\n", dev_cksum);
    }
    else
    {
        fprintf(stderr, "Checksum mismatch: device 0x%04X, host 0x%04X\n",
                dev_cksum, host_cksum);
        rc = 1;
        goto done;
    }

    if (no_run)
    {
        printf("Programming complete (staying in bootloader).\n");
    }
    else
    {
        if (isp_run_aprom() != ISP_OK)
        {
            fprintf(stderr, "RUN_APROM command failed\n");
            rc = 1;
            goto done;
        }
        printf("Programming complete. Device running APROM.\n");
    }

done:
    t_end = time(NULL);
    printf("Elapsed   : %d sec\n", (int)(t_end - t_start));

    hid_close();
    free(image);
    return rc;
}
