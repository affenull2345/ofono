/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "sprdmodem.h"

static const char *sptestmode_prefix[] = { "+SPTESTMODE:", NULL };

struct radio_settings_data {
	GAtChat *chat;
	enum ofono_radio_access_mode mode;
	int slot;
	int primary_slot;
};

static enum ofono_radio_access_mode sprd_to_ofono_rat_mode(int val)
{
	switch (val) {
	case 6:
		return OFONO_RADIO_ACCESS_MODE_LTE;
	case 10:
	case 15:
		return OFONO_RADIO_ACCESS_MODE_GSM;
	case 14:
	case 22:
		return OFONO_RADIO_ACCESS_MODE_UMTS;
	default:
		break;
	}

	return OFONO_RADIO_ACCESS_MODE_ANY;
}

static int ofono_to_sprd_rat_mode(enum ofono_radio_access_mode mode,
					bool primary)
{
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
	case OFONO_RADIO_ACCESS_MODE_LTE:
		return 6;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		return primary ? 22 : 14;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		return primary ? 15 : 10;
	default:
		break;
	}

	return -1;
}

static void sptestmode_query_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	struct radio_settings_data *rsd = cbd->user;
	enum ofono_radio_access_mode mode;
	struct ofono_error error;
	GAtResultIter iter;
	int val0, val1;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+SPTESTMODE:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &val0) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &val1) == FALSE)
		goto error;

	mode = sprd_to_ofono_rat_mode(rsd->slot == 0 ? val0 : val1);

	cb(&error, mode, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void sprd_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = rsd;

	if (g_at_chat_send(rsd->chat, "AT+SPTESTMODE?", sptestmode_prefix,
				sptestmode_query_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		g_free(cbd);
	}
}

static void sptestmode_set_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void sptestmode_query_other_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct radio_settings_data *rsd = cbd->user;
	bool primary = rsd->slot == rsd->primary_slot;
	enum ofono_radio_access_mode other_mode;
	struct ofono_error error;
	GAtResultIter iter;
	int val0, val1;
	char buf[32];

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);
		return;
	}

	cbd = cb_data_ref(cbd);

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+SPTESTMODE:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &val0) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &val1) == FALSE)
		goto error;

	other_mode = sprd_to_ofono_rat_mode(rsd->slot == 0 ? val1 : val0);
	/* An active LTE slot is always primary */
	if (other_mode == OFONO_RADIO_ACCESS_MODE_LTE)
		primary = false;

	if (rsd->slot == 0) {
		val0 = ofono_to_sprd_rat_mode(rsd->mode, primary);
		val1 = ofono_to_sprd_rat_mode(other_mode, !primary);
	} else {
		val0 = ofono_to_sprd_rat_mode(other_mode, !primary);
		val1 = ofono_to_sprd_rat_mode(rsd->mode, primary);
	}

	if (val0 < 0 || val1 < 0)
		goto error;

	snprintf(buf, sizeof(buf), "AT+SPTESTMODE=%d,%d", val0, val1);

	if (g_at_chat_send(rsd->chat, buf, sptestmode_prefix,
				sptestmode_set_cb, cbd, cb_data_unref) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	cb_data_unref(cbd);
}

static void sprd_set_rat_mode(struct ofono_radio_settings *rs,
				enum ofono_radio_access_mode mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = rsd;
	rsd->mode = mode;

	if (g_at_chat_send(rsd->chat, "AT+SPTESTMODE?", sptestmode_prefix,
			sptestmode_query_other_cb, cbd, cb_data_unref) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	cb_data_unref(cbd);
}

static void sprd_query_available_rats(struct ofono_radio_settings *rs,
			ofono_radio_settings_available_rats_query_cb_t cb,
			void *data)
{
	unsigned int available_rats;
	struct ofono_modem *modem = ofono_radio_settings_get_modem(rs);

	available_rats = OFONO_RADIO_ACCESS_MODE_GSM
				| OFONO_RADIO_ACCESS_MODE_UMTS
				| OFONO_RADIO_ACCESS_MODE_LTE;

	CALLBACK_WITH_SUCCESS(cb, available_rats, data);
}

static gboolean sprd_radio_settings_register(gpointer user)
{
	struct ofono_radio_settings *rs = user;

	ofono_radio_settings_register(rs);

	return FALSE;
}

static int sprd_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct radio_settings_data *rsd;

	rsd = g_try_new0(struct radio_settings_data, 1);
	if (rsd == NULL)
		return -ENOMEM;

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);
	g_idle_add(sprd_radio_settings_register, rs);

	return 0;
}

static void sprd_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);

	ofono_radio_settings_set_data(rs, NULL);

	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static const struct ofono_radio_settings_driver driver = {
	.name			= "sprdmodem",
	.probe			= sprd_radio_settings_probe,
	.remove			= sprd_radio_settings_remove,
	.query_rat_mode		= sprd_query_rat_mode,
	.set_rat_mode		= sprd_set_rat_mode,
	.query_available_rats	= sprd_query_available_rats,
};

void sprd_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void sprd_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
