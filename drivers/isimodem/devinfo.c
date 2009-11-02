/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include "isi.h"

#define PN_PHONE_INFO		0x1B
#define INFO_TIMEOUT		5

enum return_code {
	INFO_OK = 0x00,
	INFO_FAIL = 0x01,
	INFO_NO_NUMBER = 0x02,
	INFO_NOT_SUPPORTED = 0x03
};

enum message_id {
	INFO_SERIAL_NUMBER_READ_REQ = 0x00,
	INFO_SERIAL_NUMBER_READ_RESP = 0x01,
	INFO_VERSION_READ_REQ = 0x07,
	INFO_VERSION_READ_RESP = 0x08,
	INFO_PRODUCT_INFO_READ_REQ = 0x15,
	INFO_PRODUCT_INFO_READ_RESP = 0x16
};

enum sub_block_id {
	INFO_SB_PRODUCT_INFO_NAME = 0x01,
	INFO_SB_PRODUCT_INFO_MANUFACTURER = 0x07,
	INFO_SB_SN_IMEI_PLAIN = 0x41,
	INFO_SB_MCUSW_VERSION = 0x48
};

enum product_info_type {
	INFO_PRODUCT_NAME = 0x01,
	INFO_PRODUCT_MANUFACTURER = 0x07
};

enum serial_number_type {
	INFO_SN_IMEI_PLAIN = 0x41
};

enum version_type {
	INFO_MCUSW = 0x01
};

struct devinfo_data {
	GIsiClient *client;
};

static bool info_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_devinfo_query_cb_t cb = cbd->cb;

	GIsiSubBlockIter iter;
	char *info = NULL;
	guint8 chars;

	if(!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3) {
		DBG("Truncated message.");
		goto error;
	}

	if (msg[0] != INFO_PRODUCT_INFO_READ_RESP &&
		msg[0] != INFO_VERSION_READ_RESP &&
		msg[0] != INFO_SERIAL_NUMBER_READ_RESP) {
		DBG("Unexpected message ID: 0x%02x", msg[0]);
		goto error;
	}

	if (msg[1] != INFO_OK) {
		DBG("Request failed: 0x%02X", msg[1]);
		goto error;
	}

	for (g_isi_sb_iter_init(&iter, msg, len, 3);
	     g_isi_sb_iter_is_valid(&iter);
	     g_isi_sb_iter_next(&iter)) {
		switch (g_isi_sb_iter_get_id(&iter)) {

		case INFO_SB_PRODUCT_INFO_MANUFACTURER:
		case INFO_SB_PRODUCT_INFO_NAME:
		case INFO_SB_MCUSW_VERSION:
		case INFO_SB_SN_IMEI_PLAIN:

			if (g_isi_sb_iter_get_len(&iter) < 5)
				goto error;

			if (!g_isi_sb_iter_get_byte(&iter, &chars, 3))
				goto error;

			if (!g_isi_sb_iter_get_latin_tag(&iter,
					&info, chars, 4))
				goto error;

			DBG("info=<%s>", info);
			CALLBACK_WITH_SUCCESS(cb, info, cbd->data);
			g_free(info);
			goto out;

		default:
			DBG("Unknown sub-block: 0x%02X (%zu bytes)",
				g_isi_sb_iter_get_id(&iter),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

error:
	CALLBACK_WITH_FAILURE(cb, "", cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_query_manufacturer(struct ofono_devinfo *info,
					ofono_devinfo_query_cb_t cb,
					void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_MANUFACTURER
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				info_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, "", data);
}

static void isi_query_model(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_PRODUCT_INFO_READ_REQ,
		INFO_PRODUCT_NAME
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				info_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, "", data);
}

static void isi_query_revision(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_VERSION_READ_REQ,
		0x00, INFO_MCUSW,
		0x00, 0x00, 0x00, 0x00
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				info_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, "", data);
}

static void isi_query_serial(struct ofono_devinfo *info,
				ofono_devinfo_query_cb_t cb,
				void *data)
{
	struct devinfo_data *dev = ofono_devinfo_get_data(info);
	struct isi_cb_data *cbd = isi_cb_data_new(dev, cb, data);

	const unsigned char msg[] = {
		INFO_SERIAL_NUMBER_READ_REQ,
		INFO_SN_IMEI_PLAIN
	};

	if (!cbd)
		goto error;

	if (g_isi_request_make(dev->client, msg, sizeof(msg), INFO_TIMEOUT,
				info_resp_cb, cbd))
		return;

error:
	if (cbd)
		g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, "", data);
}

static gboolean isi_devinfo_register(gpointer user)
{
	struct ofono_devinfo *info = user;

	ofono_devinfo_register(info);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, bool alive, void *opaque)
{
	struct ofono_devinfo *info = opaque;

	if (alive == true) {
		DBG("Resource 0x%02X, with version %03d.%03d reachable",
			g_isi_client_resource(client),
			g_isi_version_major(client),
			g_isi_version_minor(client));
		g_idle_add(isi_devinfo_register, info);
		return;
	}
	DBG("Unable to bootsrap devinfo driver");
}

static int isi_devinfo_probe(struct ofono_devinfo *info, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct devinfo_data *data = g_try_new0(struct devinfo_data, 1);

	if (!data)
		return -ENOMEM;

	DBG("idx=%p", idx);

	data->client = g_isi_client_create(idx, PN_PHONE_INFO);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_devinfo_set_data(info, data);

	if (!g_isi_verify(data->client, reachable_cb, info))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_devinfo_remove(struct ofono_devinfo *info)
{
	struct devinfo_data *data = ofono_devinfo_get_data(info);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_devinfo_driver driver = {
	.name			= "isimodem",
	.probe			= isi_devinfo_probe,
	.remove			= isi_devinfo_remove,
	.query_manufacturer	= isi_query_manufacturer,
	.query_model		= isi_query_model,
	.query_revision		= isi_query_revision,
	.query_serial		= isi_query_serial
};

void isi_devinfo_init()
{
	ofono_devinfo_driver_register(&driver);
}

void isi_devinfo_exit()
{
	ofono_devinfo_driver_unregister(&driver);
}