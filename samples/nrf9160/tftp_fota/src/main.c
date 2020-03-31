/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <string.h>
#include <zephyr.h>
#include <stdlib.h>
#include <net/socket.h>
#include <modem/bsdlib.h>
#include <modem/lte_lc.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <modem/modem_key_mgmt.h>

/* Support for JSON Script */
#include <data/json.h>

/* Support for TFTP. */
#include <net/tftp.h>

/* Support for reboot */
#include <power/reboot.h>

#include <storage/flash_map.h>
#include <dfu/flash_img.h>
#include <drivers/flash.h>
#include <dfu/mcuboot.h>

/* Hardcoded values for Server IP / Port. Should be updated by the user per their config. */
#define TFTPS_IP     "117.221.140.109"
#define TFTPS_PORT   6565

/* values used below are auto-generated by pm_config.h */
#include "pm_config.h"
#define FLASH_AREA_IMAGE_PRIMARY        PM_APP_ID
#define FLASH_AREA_IMAGE_SECONDARY      PM_MCUBOOT_SECONDARY_ID
#define FLASH_BANK_SIZE                 PM_MCUBOOT_SECONDARY_SIZE


int a = 0;
struct k_sem sem;

static struct flash_img_context dfu_ctx;

/* Structure for getting the update configuration data */
struct tftp_update_cfg_data {
        int action_id;
        const char *filename;
        size_t filesize;
        u32_t poll_interval;
};

struct tftp_update_cfg_data result;

static const struct json_obj_descr json_update_cfg_data_descr[] = {
        JSON_OBJ_DESCR_PRIM(struct tftp_update_cfg_data, action_id,
                                JSON_TOK_NUMBER),
        JSON_OBJ_DESCR_PRIM(struct tftp_update_cfg_data, filename,
                                JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM(struct tftp_update_cfg_data, filesize,
                                JSON_TOK_NUMBER),
        JSON_OBJ_DESCR_PRIM(struct tftp_update_cfg_data, poll_interval,
                                JSON_TOK_NUMBER),
};


char *body_data;
u32_t body_len;

void response_cb(struct tftp_response *rsp,
                        enum tftp_final_call final_data,
                        void *user_data)
{
        int ret = 0;

        if (strcmp(user_data, "update_file_request") == 0) {
                body_data = rsp->recv_buf;
                body_len = rsp->recv_buf_len;
        }

        if (strcmp(user_data, "FOTA_PROCESS") == 0) {
                ret = flash_img_buffered_write(&dfu_ctx,
                                                rsp->recv_buf,
                                                rsp->recv_buf_len,
                                                final_data == TFTP_DATA_FINAL);
                if (ret < 0) {
                        printk("Failed to write into flash \n");
                }

                if (final_data == TFTP_DATA_FINAL) {
                        k_sem_give(&sem);
                }

        }


}

/* Initialize AT communications */
int at_comms_init(void)
{
	int err;

	err = at_cmd_init();
	if (err) {
		printk("Failed to initialize AT commands, err %d\n", err);
		return err;
	}

	err = at_notif_init();
	if (err) {
		printk("Failed to initialize AT notifications, err %d\n", err);
		return err;
	}

	return 0;
}

void main(void)
{
	int ret;
        struct tftp_request req;

	printk("TFTP FOTA client sample started\n\r");

	ret = bsdlib_init();
	if (ret) {
		printk("Failed to initialize bsdlib!");
		return;
	}

	/* Initialize AT comms in order to provision the certificate */
	ret = at_comms_init();
	if (ret) {
		return;
	}

	printk("Waiting for network.. ");
	ret = lte_lc_init_and_connect();
	if (ret) {
		printk("Failed to connect to the LTE network, err %d\n", ret);
		return;
	}


	printk(" Connected to the NB-IoT Network \n");
	
        req.response = response_cb;
        req.server.sin_family = AF_INET;
        req.server.sin_port   = htons(TFTPS_PORT);
        inet_pton(AF_INET, TFTPS_IP, &req.server.sin_addr);
        req.mode = "octet";
        req.remote_file = "update";
        req.user_data = "update_file_request";

      	ret = tftp_get(&req);

	if (ret < 0) {
		printk("Failed to send tftp_get request \n");
	}

	ret = json_obj_parse(body_data, body_len,
                        json_update_cfg_data_descr,
                        ARRAY_SIZE(json_update_cfg_data_descr),
                  	&result);

	printk("TFTP Response: %s \n", body_data);
    
	printk("action_id: %d\n", result.action_id);
        printk("filename: %s\n", result.filename);
        printk("filesize: %u\n", result.filesize);
        printk("poll interval: %u \n", result.poll_interval);

	boot_write_img_confirmed();

        ret = flash_img_init(&dfu_ctx);
        if (ret < 0) {
                printk("Failed to init flash \n");
        }

        ret = boot_erase_img_bank(FLASH_AREA_IMAGE_SECONDARY);
        if (ret < 0) {
                printk("Failed to erase flash bank 1 \n");
        }

        k_sem_init(&sem, 0, 1);

        memset(&req, 0, sizeof(req));
        req.response = response_cb;
        req.server.sin_family = AF_INET;
        req.server.sin_port   = htons(TFTPS_PORT);
        inet_pton(AF_INET, TFTPS_IP, &req.server.sin_addr);
        req.mode = "octet";
        req.remote_file = "zephyr";
        req.user_data = "FOTA_PROCESS";

        ret = tftp_get(&req);

        printk("\n file Downloaded \n");

        printk("Image write done \n");
        boot_request_upgrade(false);
        sys_reboot(0);

        while(1);
}