/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Alexander Kanavin <alexander.kanavin@nokia.com>
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
#include <ofono/call-barring.h>
#include "util.h"

#include "isi.h"
#include "ss.h"

struct barr_data {
	GIsiClient *client;
};

static bool set_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_ACTIVATION && msg[1] != SS_DEACTIVATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return true;
}


static void isi_set(struct ofono_call_barring *barr, const char *lock,
			int enable, const char *passwd, int cls,
			ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code;
	char *ucs2 = NULL;

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		enable ? SS_ACTIVATION : SS_DEACTIVATION,
		SS_ALL_TELE_AND_BEARER,
		0, 0,				/* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		1,				/* Subblock count */
		SS_GSM_PASSWORD,
		28,				/* Subblock length */
		0, 0, 0, 0, 0, 0, 0, 0,		/* Password */
		0, 0, 0, 0, 0, 0, 0, 0,		/* Filler */
		0, 0, 0, 0, 0, 0, 0, 0,		/* Filler */
		0, 0				/* Filler */
	};

	DBG("lock code %s enable %d class %d password %s\n",
		lock, enable, cls, passwd);

	if (!cbd || !passwd || strlen(passwd) > 4 || cls != 7)
		goto error;

	if (strcmp(lock, "AO") == 0)
		ss_code = SS_GSM_BARR_ALL_OUT;
	else if (strcmp(lock, "OI") == 0)
		ss_code = SS_GSM_BARR_OUT_INTER;
	else if (strcmp(lock, "OX") == 0)
		ss_code = SS_GSM_BARR_OUT_INTER_EXC_HOME;
	else if (strcmp(lock, "AI") == 0)
		ss_code = SS_GSM_BARR_ALL_IN;
	else if (strcmp(lock, "IR") == 0)
		ss_code = SS_GSM_BARR_ALL_IN_ROAM;
	else if (strcmp(lock, "AB") == 0)
		ss_code = SS_GSM_ALL_BARRINGS;
	else if (strcmp(lock, "AG") == 0)
		ss_code = SS_GSM_BARR_ALL_OUT;
	else if (strcmp(lock, "AC") == 0)
		ss_code = SS_GSM_BARR_ALL_IN;
	else
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	ucs2 = g_convert(passwd, 4, "UCS-2BE", "UTF-8//TRANSLIT",
				NULL, NULL, NULL);
	if (ucs2 == NULL)
		goto error;

	memcpy((char *)msg + 9, ucs2, 8);
	g_free(ucs2);

	if (g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
				set_resp_cb, cbd))
		return;
error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void update_status_mask(unsigned int *mask, int bsc)
{
	switch (bsc) {

	case SS_GSM_TELEPHONY:
		*mask |= 1;
		break;

	case SS_GSM_ALL_DATA_TELE:
		*mask |= 1 << 1;
		break;

	case SS_GSM_FACSIMILE:
		*mask |= 1 << 2;
		break;

	case SS_GSM_SMS:
		*mask |= 1 << 3;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_SYNC:
		*mask |= 1 << 4;
		break;

	case SS_GSM_ALL_DATA_CIRCUIT_ASYNC:
		*mask |= 1 << 5;
		break;

	case SS_GSM_ALL_DATA_PACKET_SYNC:
		*mask |= 1 << 6;
		break;

	case SS_GSM_ALL_PAD_ACCESS:
		*mask |= 1 << 7;
		break;

	default:
		DBG("Unknown BSC: 0x%04X\n", bsc);
		break;
	}
}

static bool query_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	GIsiSubBlockIter iter;
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_query_cb_t cb = cbd->cb;

	guint32 mask = 0;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 7 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_INTERROGATION)
		goto error;

	for (g_isi_sb_iter_init(&iter, msg, len, 7);
		g_isi_sb_iter_is_valid(&iter);
		g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {

		case SS_STATUS_RESULT:
			break;

		case SS_GSM_BSC_INFO: {

			guint8 count = 0;
			guint8 i;

			if (!g_isi_sb_iter_get_byte(&iter, &count, 2))
				goto error;

			for (i = 0; i < count; i++) {

				guint8 bsc = 0;

				if (!g_isi_sb_iter_get_byte(&iter, &bsc, 3 + i))
					goto error;

			        update_status_mask(&mask, bsc);
			}
			break;
		}

		case SS_GSM_ADDITIONAL_INFO:
			break;

		default:
			DBG("Skipping sub-block: 0x%04X (%zu bytes)",
				g_isi_sb_iter_get_id(&iter),
				g_isi_sb_iter_get_len(&iter));
			break;
		}
	}

	DBG("mask=0x%04X\n", mask);
	CALLBACK_WITH_SUCCESS(cb, mask, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, 0, cbd->data);

out:
	g_free(cbd);
	return true;

}

static void isi_query(struct ofono_call_barring *barr, const char *lock, int cls,
			ofono_call_barring_query_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code;

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_INTERROGATION,
		SS_ALL_TELE_AND_BEARER,
		0, 0,				/* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		0				/* Subblock count */
	};

	DBG("barring query lock code %s class %d\n", lock, cls);

	if (!cbd || cls != 7)
		goto error;

	if (strcmp(lock, "AO") == 0)
		ss_code = SS_GSM_BARR_ALL_OUT;
	else if (strcmp(lock, "OI") == 0)
		ss_code = SS_GSM_BARR_OUT_INTER;
	else if (strcmp(lock, "OX") == 0)
		ss_code = SS_GSM_BARR_OUT_INTER_EXC_HOME;
	else if (strcmp(lock, "AI") == 0)
		ss_code = SS_GSM_BARR_ALL_IN;
	else if (strcmp(lock, "IR") == 0)
		ss_code = SS_GSM_BARR_ALL_IN_ROAM;
	else
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	if (g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
				query_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, 0, data);
	g_free(cbd);
}

static bool set_passwd_resp_cb(GIsiClient *client, const void *restrict data,
				size_t len, uint16_t object, void *opaque)
{
	const unsigned char *msg = data;
	struct isi_cb_data *cbd = opaque;
	ofono_call_barring_set_cb_t cb = cbd->cb;

	if (!msg) {
		DBG("ISI client error: %d", g_isi_client_error(client));
		goto error;
	}

	if (len < 3 || msg[0] != SS_SERVICE_COMPLETED_RESP)
		goto error;

	if (msg[1] != SS_GSM_PASSWORD_REGISTRATION)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	goto out;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

out:
	g_free(cbd);
	return true;
}

static void isi_set_passwd(struct ofono_call_barring *barr, const char *lock,
				const char *old_passwd, const char *new_passwd,
				ofono_call_barring_set_cb_t cb, void *data)
{
	struct barr_data *bd = ofono_call_barring_get_data(barr);
	struct isi_cb_data *cbd = isi_cb_data_new(barr, cb, data);
	int ss_code;
	char *ucs2 = NULL;

	unsigned char msg[] = {
		SS_SERVICE_REQ,
		SS_GSM_PASSWORD_REGISTRATION,
		SS_ALL_TELE_AND_BEARER,
		0, 0,				/* Supplementary services code */
		SS_SEND_ADDITIONAL_INFO,
		1,				/* Subblock count */
		SS_GSM_PASSWORD,
		28,				/* Subblock length */
		0, 0, 0, 0, 0, 0, 0, 0,		/* Old password */
		0, 0, 0, 0, 0, 0, 0, 0,		/* New password */
		0, 0, 0, 0, 0, 0, 0, 0,		/* New password */
		0, 0				/* Filler */
	};

	if (!cbd || strlen(old_passwd) > 4 || strlen(new_passwd) > 4)
		goto error;

	DBG("lock code %s old password %s new password %s\n",
		lock, old_passwd, new_passwd);

	if (strcmp(lock, "AB") == 0)
		ss_code = SS_GSM_ALL_BARRINGS;
	else
		goto error;

	msg[3] = ss_code >> 8;
	msg[4] = ss_code & 0xFF;

	ucs2 = g_convert(old_passwd, 4, "UCS-2BE", "UTF-8//TRANSLIT",
				NULL, NULL, NULL);
	if (ucs2 == NULL)
		goto error;

	memcpy((char *)msg + 9, ucs2, 8);
	g_free(ucs2);

	ucs2 = g_convert(new_passwd, 4, "UCS-2BE", "UTF-8//TRANSLIT",
				NULL, NULL, NULL);
	if (ucs2 == NULL)
		goto error;

	memcpy((char *)msg + 17, ucs2, 8);
	memcpy((char *)msg + 25, ucs2, 8);
	g_free(ucs2);

	if (g_isi_request_make(bd->client, msg, sizeof(msg), SS_TIMEOUT,
				set_passwd_resp_cb, cbd))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static gboolean isi_call_barring_register(gpointer user)
{
	struct ofono_call_barring *cb = user;

	ofono_call_barring_register(cb);

	return FALSE;
}

static void reachable_cb(GIsiClient *client, bool alive, void *opaque)
{
	struct ofono_call_barring *barr = opaque;

	if (alive == true) {
		DBG("Resource 0x%02X, with version %03d.%03d reachable",
			g_isi_client_resource(client),
			g_isi_version_major(client),
			g_isi_version_minor(client));
		g_idle_add(isi_call_barring_register, barr);
		return;
	}
	DBG("Unable to bootsrap call barring driver");
}


static int isi_call_barring_probe(struct ofono_call_barring *barr,
					unsigned int vendor, void *user)
{
	GIsiModem *idx = user;
	struct barr_data *data = g_try_new0(struct barr_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_SS);
	if (!data->client)
		return -ENOMEM;

	ofono_call_barring_set_data(barr, data);
	if (!g_isi_verify(data->client, reachable_cb, barr))
		DBG("Unable to verify reachability");

	return 0;
}

static void isi_call_barring_remove(struct ofono_call_barring *barr)
{
	struct barr_data *data = ofono_call_barring_get_data(barr);

	if (data) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_call_barring_driver driver = {
	.name			= "isimodem",
	.probe			= isi_call_barring_probe,
	.remove			= isi_call_barring_remove,
	.set			= isi_set,
	.query			= isi_query,
	.set_passwd		= isi_set_passwd
};

void isi_call_barring_init()
{
	ofono_call_barring_driver_register(&driver);
}

void isi_call_barring_exit()
{
	ofono_call_barring_driver_unregister(&driver);
}