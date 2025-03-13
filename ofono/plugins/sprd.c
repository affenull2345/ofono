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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/conf.h>
#include <ofono/devinfo.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/slot.h>
#include <ofono/sms.h>
#include <ofono/storage.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct sprd_data {
	GAtChat *at;
	struct at_util_sim_state_query *sim_state_query;
	bool have_sim;
	char *imei, *imeisv;
};

typedef struct ofono_slot_driver_data {
	struct ofono_slot_manager *slot_manager;
	GSList *slots;
	bool started;
} SprdPlugin;

typedef struct sprd_slot_data {
	struct ofono_slot *handle;
	struct ofono_modem *modem;
	SprdPlugin *plugin;
	uint id;
} SprdSlot;

static SprdPlugin *global_sprd_slot_plugin = NULL;

static void sprd_slot_free(SprdSlot *slot);
static void sprd_slot_driver_modem_ready(struct ofono_modem *modem);

static void sprd_debug(const char *str, void *user_data)
{
	struct ofono_modem *modem = user_data;

	ofono_info("%s %s", ofono_modem_get_path(modem), str);
}

static GAtChat *open_device(struct ofono_modem *modem, const char *key)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (!device)
		return NULL;

	DBG("%s %s", key, device);

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, sprd_debug, modem);

	return chat;
}

static int sprd_probe(struct ofono_modem *modem)
{
	struct sprd_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct sprd_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void sprd_remove(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);
	GSList *link;

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->at);

	if (global_sprd_slot_plugin) {
		link = global_sprd_slot_plugin->slots;
		for (; link; link = link->next) {
			SprdSlot *slot = link->data;

			if (slot->modem == modem)
				slot->modem = NULL;
		}
	}

	g_free(data->imeisv);
	g_free(data->imei);
	g_free(data);
}

/*
 * FIXME: this IMEI querying code is currently duplicate. The IMEI is also
 * queried by the devinfo driver, but it does not indicate when the query
 * is complete.
 */
static void query_imeisv_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sprd_data *data = ofono_modem_get_data(modem);
	const char *imeisv;

	if (!ok) {
		ofono_error("Failed to query IMEI SV");
		return;
	}

	if (!at_util_parse_attr(result, "+SGMR:", &imeisv)) {
		ofono_error("Failed to parse IMEI SV");
		return;
	}

	data->imeisv = g_strdup(imeisv);

	ofono_modem_set_powered(modem, true);
	sprd_slot_driver_modem_ready(modem);
}

static void query_imei_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sprd_data *data = ofono_modem_get_data(modem);
	const char *imei;

	if (!ok) {
		ofono_error("Failed to query IMEI");
		return;
	}

	if (!at_util_parse_attr(result, "+CGSN:", &imei)) {
		ofono_error("Failed to parse IMEI");
		return;
	}

	data->imei = g_strdup(imei);

	g_at_chat_send(data->at, "AT+SGMR=0,0,2", NULL, query_imeisv_cb,
			modem, NULL);
}

static void sim_state_cb(gboolean present, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sprd_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	at_util_sim_state_query_free(data->sim_state_query);
	data->sim_state_query = NULL;

	data->have_sim = present;

	g_at_chat_send(data->at, "AT+CGSN", NULL, query_imei_cb, modem, NULL);
}

static int sprd_enable(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->at = open_device(modem, "AT");
	if (!data->at)
		return -EINVAL;

	/* Configure default AT settings */
	g_at_chat_send(data->at, "ATE0Q0V1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->at, "AT+CMEE=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->at, "AT+CGPIAF=1", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(data->at, "AT+SMMSWAP=0", none_prefix, NULL, NULL, NULL);

	data->sim_state_query = at_util_sim_state_query_new(data->at,
						2, 20, sim_state_cb, modem,
						NULL);

	return -EINPROGRESS;
}

static void cfun_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct sprd_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->at);
	data->at = NULL;

	if (ok)
		ofono_modem_set_powered(modem, false);
}

static int sprd_disable(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->at);
	g_at_chat_unregister_all(data->at);

	g_at_chat_send(data->at, "AT+SFUN=5", none_prefix,
				cfun_disable_cb, modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void sprd_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct sprd_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+SFUN=4" : "AT+SFUN=5";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->at, command, none_prefix,
					set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void sprd_pre_sim(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, OFONO_VENDOR_SPRD, "atmodem", data->at);
	sim = ofono_sim_create(modem, OFONO_VENDOR_SPRD, "atmodem", data->at);
	ofono_voicecall_create(modem, OFONO_VENDOR_SPRD, "atmodem", data->at);

	if (sim && data->have_sim)
		ofono_sim_inserted_notify(sim, true);
}

static void sprd_post_sim(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", data->at);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->at);
	ofono_call_settings_create(modem, 0, "atmodem", data->at);
	ofono_call_barring_create(modem, 0, "atmodem", data->at);
	ofono_phonebook_create(modem, 0, "generic", modem);
	ofono_radio_settings_create(modem, 0, "sprdmodem", data->at);
	ofono_sms_create(modem, OFONO_VENDOR_SPRD, "atmodem", data->at);
	ofono_cbs_create(modem, 0, "atmodem", data->at);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->at);
	gc = ofono_gprs_context_create(modem, 0, "sprdmodem", data->at);

	if (gc) {
		ofono_gprs_context_set_type(gc,
					OFONO_GPRS_CONTEXT_TYPE_INTERNET);
		ofono_gprs_add_context(gprs, gc);
	}

	gc = ofono_gprs_context_create(modem, 0, "sprdmodem", data->at);

	if (gc) {
		ofono_gprs_context_set_type(gc, OFONO_GPRS_CONTEXT_TYPE_MMS);
		ofono_gprs_add_context(gprs, gc);
	}
}


static void sprd_post_online(struct ofono_modem *modem)
{
	struct sprd_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_SPRD, "atmodem", data->at);
}

static struct ofono_modem_driver sprd_driver = {
	.name		= "sprd",
	.probe		= sprd_probe,
	.remove		= sprd_remove,
	.enable		= sprd_enable,
	.disable	= sprd_disable,
	.set_online	= sprd_set_online,
	.pre_sim	= sprd_pre_sim,
	.post_sim	= sprd_post_sim,
	.post_online	= sprd_post_online,
};

static struct ofono_slot_driver_reg *sprd_slot_driver_reg = NULL;

static void sprd_slot_free(SprdSlot *slot)
{
	g_free(slot);
}

static void sprd_slot_driver_startup_check(SprdPlugin *plugin)
{
	GSList *link;

	if (plugin->started) {
		DBG("already started");
		return;
	}

	for (link = plugin->slots; link; link = link->next) {
		SprdSlot *slot = link->data;

		if (!slot->modem) {
			DBG("slot %u has no modem yet", slot->id);
			return;
		}
	}

	plugin->started = true;

	for (link = plugin->slots; link; link = link->next) {
		SprdSlot *slot = link->data;
		struct sprd_data *data = ofono_modem_get_data(slot->modem);

		ofono_slot_add(plugin->slot_manager,
				ofono_modem_get_path(slot->modem),
				(OFONO_RADIO_ACCESS_MODE_GSM |
					OFONO_RADIO_ACCESS_MODE_UMTS |
					OFONO_RADIO_ACCESS_MODE_LTE),
				data->imei, data->imeisv,
				data->have_sim ? OFONO_SLOT_SIM_PRESENT :
					OFONO_SLOT_SIM_ABSENT,
				OFONO_SLOT_NO_FLAGS);
	}

	ofono_slot_driver_started(sprd_slot_driver_reg);

	DBG("startup complete");
}

static ofono_bool_t sprd_slot_modem_match(struct ofono_modem *modem,
						void *user_data)
{
	SprdSlot *slot = user_data;
	char path[32];

	snprintf(path, sizeof(path), "/embedded/sprd/%u", slot->id);
	if (!g_strcmp0(ofono_modem_get_string(modem, "SystemPath"), path))
		return true;

	return false;
}

static struct ofono_modem *sprd_slot_find_ready_modem(SprdSlot *slot)
{
	struct ofono_modem *modem;

	modem = ofono_modem_find(sprd_slot_modem_match, slot);
	if (!modem) {
		DBG("%u: modem not found", slot->id);
		return NULL;
	}

	if (!ofono_modem_get_powered(modem)) {
		DBG("%u: modem not ready", slot->id);
		return NULL;
	}

	return modem;
}

static void sprd_slot_driver_modem_ready(struct ofono_modem *modem)
{
	GSList *link;

	if (!global_sprd_slot_plugin) {
		DBG("plugin not initialized yet");
		return;
	}

	for (link = global_sprd_slot_plugin->slots; link; link = link->next) {
		SprdSlot *slot = link->data;

		if (slot->modem)
			continue;

		if (sprd_slot_modem_match(modem, slot)) {
			slot->modem = modem;
			break;
		}
	}

	sprd_slot_driver_startup_check(global_sprd_slot_plugin);
}

static GSList *sprd_slot_driver_add_slot(GSList *slots, SprdSlot *new_slot)
{
	GSList *link = slots;

	DBG("Adding slot %u", new_slot->id);

	while (link) {
		GSList *next = link->next;
		SprdSlot *slot = link->data;

		if (slot->id == new_slot->id) {
			ofono_error("Duplicate slot %u", slot->id);
			slots = g_slist_delete_link(slots, link);
			sprd_slot_free(slot);
		}

		link = next;
	}

	return g_slist_append(slots, new_slot);
}

static void sprd_slot_driver_load_config(SprdPlugin *plugin,
		const char *path)
{
	GKeyFile *file = g_key_file_new();
	char **slots;
	GSList *list = NULL;

	g_key_file_set_list_separator(file, ',');
	ofono_conf_merge_files(file, path);

	slots = ofono_conf_get_strings(file, OFONO_COMMON_SETTINGS_GROUP,
					"Slots", ',');
	if (slots) {
		char **p, *endp;
		unsigned int id;
		SprdSlot *slot;

		for (p = slots; *p; p++) {
			id = strtoul(*p, &endp, 10);
			if (*endp != '\0')
				continue;

			slot = g_new0(SprdSlot, 1);
			slot->id = id;
			slot->plugin = plugin;
			slot->modem = sprd_slot_find_ready_modem(slot);

			list = sprd_slot_driver_add_slot(list, slot);
		}

		g_strfreev(slots);
	}

	plugin->slots = list;
}

static SprdPlugin *sprd_slot_driver_init(struct ofono_slot_manager *m)
{
	SprdPlugin *plugin;
	char *config_file = g_build_filename(ofono_config_dir(),
			"sprd.conf", NULL);

	DBG("");

	plugin = g_new0(SprdPlugin, 1);
	plugin->slot_manager = m;

	sprd_slot_driver_load_config(plugin, config_file);
	g_free(config_file);

	if (global_sprd_slot_plugin)
		ofono_error("sprd slot plugin already initialized");
	else
		global_sprd_slot_plugin = plugin;

	return plugin;
}

static uint sprd_slot_driver_start(SprdPlugin *plugin)
{
	DBG("");
	sprd_slot_driver_startup_check(plugin);
	return 1;
}

static void sprd_slot_driver_cancel(SprdPlugin *plugin, uint id)
{
	DBG("%d", id);
}

static void sprd_slot_driver_cleanup(SprdPlugin *plugin)
{
	DBG("");
	g_free(plugin);
	global_sprd_slot_plugin = NULL;
}

static const struct ofono_slot_driver sprd_slot_driver = {
	.name = "sprd_slot",
	.api_version = OFONO_SLOT_API_VERSION,
	.init = sprd_slot_driver_init,
	.start = sprd_slot_driver_start,
	.cancel = sprd_slot_driver_cancel,
	.cleanup = sprd_slot_driver_cleanup,
};

static int sprd_init(void)
{
	sprd_slot_driver_reg = ofono_slot_driver_register(&sprd_slot_driver);

	return ofono_modem_driver_register(&sprd_driver);
}

static void sprd_exit(void)
{
	ofono_slot_driver_unregister(sprd_slot_driver_reg);
	sprd_slot_driver_reg = NULL;

	ofono_modem_driver_unregister(&sprd_driver);
}

OFONO_PLUGIN_DEFINE(sprd, "Unisoc/Spreadtrum modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, sprd_init, sprd_exit)
