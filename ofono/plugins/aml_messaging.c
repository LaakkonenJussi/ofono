/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2021  Jolla Ltd. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>
#include <ofono/sms.h>
#include "smsutil.h"
#include "common.h"

#define AML_SRC_PORT -1
#define AML_DST_PORT 6666

static unsigned int modemwatch_id;

struct aml_messaging {
	struct ofono_modem *modem;
	struct ofono_sms *sms;
	unsigned int recv_watch;
};

static void aml_received(const char *from, const struct tm *remote,
				const struct tm *local, int dst, int src,
				const unsigned char *buffer,
				unsigned int len, void *data)
{
	struct aml_messaging *am = data;
	char *buf;
	unsigned int written;

	DBG("%p", am);

	buf = g_malloc0(sizeof(char) * 161);

	if ((written = ofono_unpack_7bit(buffer, len, 0x01, buf, 161)))
		DBG("unpack %d bytes: \"%s\"", written, buf);
	else
		DBG("failed to unpack");
}

static void aml_messaging_cleanup(gpointer user)
{
	struct aml_messaging *am = user;

	DBG("%p", am);

	if (am->recv_watch)
		__ofono_sms_datagram_watch_remove(am->sms, am->recv_watch);

	am->recv_watch = 0;
	am->sms = NULL;
}

static void sms_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct aml_messaging *am = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		if (__ofono_sms_datagram_watch_remove(am->sms,
							am->recv_watch)) {
			am->recv_watch = 0;
			DBG("recv unregistered");
		}

		return;
	}

	DBG("registered");
	am->sms = __ofono_atom_get_data(atom);
	am->recv_watch = __ofono_sms_datagram_watch_add(am->sms, aml_received,
					AML_DST_PORT, AML_SRC_PORT, am, NULL);
}

static void modem_watch(struct ofono_modem *modem, gboolean added, void *user)
{
	struct aml_messaging *am;
	DBG("modem: %p, added: %d", modem, added);

	if (added == FALSE)
		return;

	am = g_try_new0(struct aml_messaging, 1);
	if (am == NULL)
		return;

	am->modem = modem;
	__ofono_modem_add_atom_watch(modem, OFONO_ATOM_TYPE_SMS,
					sms_watch, am, aml_messaging_cleanup);
}

static void call_modemwatch(struct ofono_modem *modem, void *user)
{
	modem_watch(modem, TRUE, user);
}

static int aml_messaging_init(void)
{
	DBG("");

	modemwatch_id = __ofono_modemwatch_add(modem_watch, NULL, NULL);

	__ofono_modem_foreach(call_modemwatch, NULL);

	return 0;
}

static void aml_messaging_exit(void)
{
	DBG("");

	__ofono_modemwatch_remove(modemwatch_id);
}

OFONO_PLUGIN_DEFINE(aml_messaging, "AML Messaging Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			aml_messaging_init, aml_messaging_exit)
