/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012  Instituto Nokia de Tecnologia - INdT
 *  Copyright (C) 2014  Google Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/sdp.h"
#include "lib/uuid.h"

#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"
#include "src/plugin.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/log.h"

#define GAP_UUID16 0x1800

/* Generic Attribute/Access Service */
struct gas {
	struct btd_device *device;
	struct gatt_db *db;
	unsigned int db_id;
	struct bt_gatt_client *client;
	struct gatt_db_attribute *attr;
};

static GSList *devices;

static void gas_free(struct gas *gas)
{
	gatt_db_unregister(gas->db, gas->db_id);
	gatt_db_unref(gas->db);
	bt_gatt_client_unref(gas->client);
	btd_device_unref(gas->device);
	g_free(gas);
}

static int cmp_device(gconstpointer a, gconstpointer b)
{
	const struct gas *gas = a;
	const struct btd_device *device = b;

	return gas->device == device ? 0 : -1;
}

static char *name2utf8(const uint8_t *name, uint16_t len)
{
	char utf8_name[HCI_MAX_NAME_LENGTH + 2];
	int i;

	if (g_utf8_validate((const char *) name, len, NULL))
		return g_strndup((char *) name, len);

	len = MIN(len, sizeof(utf8_name) - 1);

	memset(utf8_name, 0, sizeof(utf8_name));
	strncpy(utf8_name, (char *) name, len);

	/* Assume ASCII, and replace all non-ASCII with spaces */
	for (i = 0; utf8_name[i] != '\0'; i++) {
		if (!isascii(utf8_name[i]))
			utf8_name[i] = ' ';
	}

	/* Remove leading and trailing whitespace characters */
	g_strstrip(utf8_name);

	return g_strdup(utf8_name);
}

static void read_device_name_cb(bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data)
{
	struct gas *gas = user_data;
	char *name;

	if (!success) {
		DBG("Reading device name failed with ATT errror: %u",
								att_ecode);
		return;
	}

	if (!length)
		return;

	name = name2utf8(value, length);

	DBG("GAP Device Name: %s", name);

	btd_device_device_set_name(gas->device, name);

	g_free(name);
}

static void handle_device_name(struct gas *gas, uint16_t value_handle)
{
	if (!bt_gatt_client_read_long_value(gas->client, value_handle, 0,
						read_device_name_cb, gas, NULL))
		DBG("Failed to send request to read device name");
}

static void read_appearance_cb(bool success, uint8_t att_ecode,
					const uint8_t *value, uint16_t length,
					void *user_data)
{
	struct gas *gas = user_data;
	uint16_t appearance;

	if (!success) {
		DBG("Reading appearance failed with ATT error: %u", att_ecode);
		return;
	}

	/* The appearance value is a 16-bit unsigned integer */
	if (length != 2) {
		DBG("Malformed appearance value");
		return;
	}

	appearance = get_le16(value);

	DBG("GAP Appearance: 0x%04x", appearance);

	device_set_appearance(gas->device, appearance);
}

static void handle_appearance(struct gas *gas, uint16_t value_handle)
{
	if (!bt_gatt_client_read_value(gas->client, value_handle,
						read_appearance_cb, gas, NULL))
		DBG("Failed to send request to read appearance");
}

static bool uuid_cmp(uint16_t u16, const bt_uuid_t *uuid)
{
	bt_uuid_t lhs;

	bt_uuid16_create(&lhs, u16);

	return bt_uuid_cmp(&lhs, uuid) == 0;
}

static void handle_characteristic(struct gatt_db_attribute *attr,
								void *user_data)
{
	struct gas *gas = user_data;
	uint16_t value_handle;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, NULL, &value_handle, NULL,
								&uuid)) {
		error("Failed to obtain characteristic data");
		return;
	}

	if (uuid_cmp(GATT_CHARAC_DEVICE_NAME, &uuid))
		handle_device_name(gas, value_handle);
	else if (uuid_cmp(GATT_CHARAC_APPEARANCE, &uuid))
		handle_appearance(gas, value_handle);
	else {
		char uuid_str[MAX_LEN_UUID_STR];

		/* TODO: Support peripheral privacy feature */

		bt_uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));
		DBG("Unsupported characteristic: %s", uuid_str);
	}
}

static void handle_gap_service(struct gas *gas)
{
	gatt_db_service_foreach_char(gas->attr, handle_characteristic, gas);
}

static int gap_driver_probe(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct gas *gas;
	GSList *l;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("GAP profile probe (%s)", addr);

	/* Ignore, if we were probed for this device already */
	l = g_slist_find_custom(devices, device, cmp_device);
	if (l) {
		error("Profile probed twice for the same device!");
		return -1;
	}

	gas = g_new0(struct gas, 1);
	if (!gas)
		return -1;

	gas->device = btd_device_ref(device);
	devices = g_slist_append(devices, gas);

	return 0;
}

static void gap_driver_remove(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct gas *gas;
	GSList *l;
	char addr[18];

	ba2str(device_get_address(device), addr);
	DBG("GAP profile remove (%s)", addr);

	l = g_slist_find_custom(devices, device, cmp_device);
	if (!l) {
		error("GAP service not handled by profile");
		return;
	}

	gas = l->data;

	devices = g_slist_remove(devices, gas);
	gas_free(gas);
}

static void foreach_gap_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct gas *gas = user_data;

	if (gas->attr) {
		error("More than one GAP service exists for this device");
		return;
	}

	gas->attr = attr;
	handle_gap_service(gas);
}

static void service_added(struct gatt_db_attribute *attr, void *user_data)
{
	struct gas *gas = user_data;
	bt_uuid_t uuid, gap_uuid;

	if (!bt_gatt_client_is_ready(gas->client))
		return;

	gatt_db_attribute_get_service_uuid(attr, &uuid);
	bt_uuid16_create(&gap_uuid, GAP_UUID16);

	if (bt_uuid_cmp(&uuid, &gap_uuid))
		return;

	if (gas->attr) {
		error("More than one GAP service added to device");
		return;
	}

	DBG("GAP service added");

	gas->attr = attr;
	handle_gap_service(gas);
}

static void service_removed(struct gatt_db_attribute *attr, void *user_data)
{
	struct gas *gas = user_data;

	if (gas->attr != attr)
		return;

	DBG("GAP service removed");

	gas->attr = NULL;
}

static int gap_driver_accept(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	struct gatt_db *db = btd_device_get_gatt_db(device);
	struct bt_gatt_client *client = btd_device_get_gatt_client(device);
	struct gas *gas;
	GSList *l;
	char addr[18];
	bt_uuid_t gap_uuid;

	ba2str(device_get_address(device), addr);
	DBG("GAP profile accept (%s)", addr);

	l = g_slist_find_custom(devices, device, cmp_device);
	if (!l) {
		error("GAP service not handled by profile");
		return -1;
	}

	gas = l->data;

	/* Clean-up any old client/db and acquire the new ones */
	gas->attr = NULL;
	gatt_db_unregister(gas->db, gas->db_id);
	gatt_db_unref(gas->db);
	bt_gatt_client_unref(gas->client);

	gas->db = gatt_db_ref(db);
	gas->client = bt_gatt_client_ref(client);
	gas->db_id = gatt_db_register(db, service_added, service_removed, gas,
									NULL);

	/* Handle the GAP services */
	bt_uuid16_create(&gap_uuid, GAP_UUID16);
	gatt_db_foreach_service(db, &gap_uuid, foreach_gap_service, gas);

	return 0;
}

static struct btd_profile gap_profile = {
	.name		= "gap-profile",
	.remote_uuid	= GAP_UUID,
	.device_probe	= gap_driver_probe,
	.device_remove	= gap_driver_remove,
	.accept		= gap_driver_accept
};

static int gap_init(void)
{
	devices = NULL;

	btd_profile_register(&gap_profile);

	return 0;
}

static void gap_exit(void)
{
	btd_profile_unregister(&gap_profile);
}

BLUETOOTH_PLUGIN_DEFINE(gap, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
							gap_init, gap_exit)
