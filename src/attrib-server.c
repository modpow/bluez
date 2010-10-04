/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
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
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "log.h"
#include "glib-helper.h"
#include "btio.h"
#include "sdpd.h"
#include "att.h"
#include "gattrib.h"

#include "attrib-server.h"

#define GATT_PSM 0x1f
#define GATT_CID 4

static GSList *database = NULL;

struct gatt_channel {
	bdaddr_t src;
	bdaddr_t dst;
	GAttrib *attrib;
	guint mtu;
	guint id;
};

struct group_elem {
	uint16_t handle;
	uint16_t end;
	uint8_t *data;
	uint16_t len;
};

static GIOChannel *l2cap_io = NULL;
static GIOChannel *le_io = NULL;
static GSList *clients = NULL;
static uint32_t sdp_handle = 0;

static uuid_t prim_uuid = { .type = SDP_UUID16, .value.uuid16 = GATT_PRIM_SVC_UUID };
static uuid_t snd_uuid = { .type = SDP_UUID16, .value.uuid16 = GATT_SND_SVC_UUID };

static sdp_record_t *server_record_new(void)
{
	sdp_list_t *svclass_id, *apseq, *proto[2], *profiles, *root, *aproto;
	uuid_t root_uuid, proto_uuid, gatt_uuid, l2cap;
	sdp_profile_desc_t profile;
	sdp_record_t *record;
	sdp_data_t *psm, *sh, *eh;
	uint16_t lp = GATT_PSM, start = 0x0001, end = 0xffff;

	record = sdp_record_alloc();
	if (record == NULL)
		return NULL;

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);
	sdp_list_free(root, NULL);

	sdp_uuid16_create(&gatt_uuid, GENERIC_ATTRIB_SVCLASS_ID);
	svclass_id = sdp_list_append(NULL, &gatt_uuid);
	sdp_set_service_classes(record, svclass_id);
	sdp_list_free(svclass_id, NULL);

	sdp_uuid16_create(&profile.uuid, GENERIC_ATTRIB_PROFILE_ID);
	profile.version = 0x0100;
	profiles = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(record, profiles);
	sdp_list_free(profiles, NULL);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	psm = sdp_data_alloc(SDP_UINT16, &lp);
	proto[0] = sdp_list_append(proto[0], psm);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&proto_uuid, ATT_UUID);
	proto[1] = sdp_list_append(NULL, &proto_uuid);
	sh = sdp_data_alloc(SDP_UINT16, &start);
	proto[1] = sdp_list_append(proto[1], sh);
	eh = sdp_data_alloc(SDP_UINT16, &end);
	proto[1] = sdp_list_append(proto[1], eh);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);

	sdp_set_info_attr(record, "Generic Attribute Profile", "BlueZ", NULL);

	sdp_set_url_attr(record, "http://www.bluez.org/",
			"http://www.bluez.org/", "http://www.bluez.org/");

	sdp_set_service_id(record, gatt_uuid);

	sdp_data_free(psm);
	sdp_data_free(sh);
	sdp_data_free(eh);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(aproto, NULL);

	return record;
}

static uint16_t read_by_group(uint16_t start, uint16_t end, uuid_t *uuid,
							uint8_t *pdu, int len)
{
	struct att_data_list *adl;
	struct attribute *a;
	struct group_elem *cur, *old = NULL;
	GSList *l, *groups;
	uint16_t length, last_handle, last_size = 0;
	int i;

	if (start > end || start == 0x0000)
		return enc_error_resp(ATT_OP_READ_BY_GROUP_REQ, start,
					ATT_ECODE_INVALID_HANDLE, pdu, len);

	/*
	 * Only <<Primary Service>> and <<Secondary Service>> grouping
	 * types may be used in the Read By Group Type Request.
	 */

	if (sdp_uuid_cmp(uuid, &prim_uuid) != 0 &&
		sdp_uuid_cmp(uuid, &snd_uuid) != 0)
		return enc_error_resp(ATT_OP_READ_BY_GROUP_REQ, 0x0000,
					ATT_ECODE_UNSUPP_GRP_TYPE, pdu, len);

	last_handle = end;
	for (l = database, groups = NULL; l; l = l->next) {
		a = l->data;

		if (a->handle < start)
			continue;

		if (a->handle >= end)
			break;

		/* The old group ends when a new one starts */
		if (old && (sdp_uuid_cmp(&a->uuid, &prim_uuid) == 0 ||
				sdp_uuid_cmp(&a->uuid, &snd_uuid) == 0)) {
			old->end = last_handle;
			old = NULL;
		}

		if (sdp_uuid_cmp(&a->uuid, uuid) != 0) {
			/* Still inside a service, update its last handle */
			if (old)
				last_handle = a->handle;
			continue;
		}

		if (last_size && (last_size != a->len))
			break;

		cur = g_new0(struct group_elem, 1);
		cur->handle = a->handle;
		cur->data = a->data;
		cur->len = a->len;

		/* Attribute Grouping Type found */
		groups = g_slist_append(groups, cur);

		last_size = a->len;
		old = cur;
	}

	if (groups == NULL)
		return enc_error_resp(ATT_OP_READ_BY_GROUP_REQ, start,
					ATT_ECODE_ATTR_NOT_FOUND, pdu, len);

	if (l == NULL)
		cur->end = a->handle;
	else
		cur->end = last_handle;

	length = g_slist_length(groups);

	adl = g_new0(struct att_data_list, 1);
	adl->len = last_size + 4;	/* Length of each element */
	adl->num = length;	/* Number of primary or secondary services */
	adl->data = g_malloc(length * sizeof(uint8_t *));

	for (i = 0, l = groups; l; l = l->next, i++) {
		uint8_t *value;

		cur = l->data;

		adl->data[i] = g_malloc(adl->len);
		value = (void *) adl->data[i];

		att_put_u16(cur->handle, value);
		att_put_u16(cur->end, &value[2]);
		/* Attribute Value */
		memcpy(&value[4], cur->data, cur->len);
	}

	length = enc_read_by_grp_resp(adl, pdu, len);

	att_data_list_free(adl);
	g_slist_foreach(groups, (GFunc) g_free, NULL);
	g_slist_free(groups);

	return length;
}

static uint16_t read_by_type(uint16_t start, uint16_t end, uuid_t *uuid,
							uint8_t *pdu, int len)
{
	struct att_data_list *adl;
	GSList *l, *types;
	struct attribute *a;
	uint16_t num, length;
	int i;

	if (start > end || start == 0x0000)
		return enc_error_resp(ATT_OP_READ_BY_TYPE_REQ, start,
					ATT_ECODE_INVALID_HANDLE, pdu, len);

	for (l = database, length = 0, types = NULL; l; l = l->next) {
		a = l->data;

		if (a->handle < start)
			continue;

		if (a->handle >= end)
			break;

		if (sdp_uuid_cmp(&a->uuid, uuid)  != 0)
			continue;

		/* All elements must have the same length */
		if (length == 0)
			length = a->len;
		else if (a->len != length)
			break;

		types = g_slist_append(types, a);
	}

	if (types == NULL)
		return enc_error_resp(ATT_OP_READ_BY_TYPE_REQ, start,
					ATT_ECODE_ATTR_NOT_FOUND, pdu, len);

	num = g_slist_length(types);

	/* Handle length plus attribute value length */
	length += 2;

	adl = g_new0(struct att_data_list, 1);
	adl->len = length;	/* Length of each element */
	adl->num = num;		/* Number of primary or secondary services */
	adl->data = g_malloc(num * sizeof(uint8_t *));

	for (i = 0, l = types; l; i++, l = l->next) {
		uint8_t *value;

		a = l->data;
		adl->data[i] = g_malloc(length);

		value = (void *) adl->data[i];

		att_put_u16(a->handle, value);

		/* Attribute Value */
		memcpy(&value[2], a->data, a->len);
	}

	length = enc_read_by_type_resp(adl, pdu, len);

	att_data_list_free(adl);
	g_slist_free(types);

	return length;
}

static int find_info(uint16_t start, uint16_t end, uint8_t *pdu, int len)
{
	struct attribute *a;
	struct att_data_list *adl;
	GSList *l, *info;
	uint8_t format, last_type = SDP_UUID_UNSPEC;
	uint16_t length, num;
	int i;

	if (start > end || start == 0x0000)
		return enc_error_resp(ATT_OP_FIND_INFO_REQ, start,
					ATT_ECODE_INVALID_HANDLE, pdu, len);

	for (l = database, info = NULL, num = 0; l; l = l->next) {
		a = l->data;

		if (a->handle < start)
			continue;

		if (a->handle > end)
			break;

		if (last_type == SDP_UUID_UNSPEC)
			last_type = a->uuid.type;

		if (a->uuid.type != last_type)
			break;

		info = g_slist_append(info, a);
		num++;

		last_type = a->uuid.type;
	}

	if (info == NULL)
		return enc_error_resp(ATT_OP_FIND_INFO_REQ, start,
					ATT_ECODE_ATTR_NOT_FOUND, pdu, len);

	if (last_type == SDP_UUID16) {
		length = 2;
		format = 0x01;
	} else if (last_type == SDP_UUID128) {
		length = 16;
		format = 0x02;
	}

	adl = g_new0(struct att_data_list, 1);
	adl->len = length + 2;	/* Length of each element */
	adl->num = num;		/* Number of primary or secondary services */
	adl->data = g_malloc(num * sizeof(uint8_t *));

	for (i = 0, l = info; l; i++, l = l->next) {
		uint8_t *value;

		a = l->data;
		adl->data[i] = g_malloc(adl->len);

		value = (void *) adl->data[i];

		att_put_u16(a->handle, value);

		/* Attribute Value */
		memcpy(&value[2], &a->uuid.value, length);
	}

	length = enc_find_info_resp(format, adl, pdu, len);

	att_data_list_free(adl);
	g_slist_free(info);

	return length;
}

static int handle_cmp(gconstpointer a, gconstpointer b)
{
	const struct attribute *attrib = a;
	uint16_t handle = GPOINTER_TO_UINT(b);

	return attrib->handle - handle;
}

static int attribute_cmp(gconstpointer a1, gconstpointer a2)
{
	const struct attribute *attrib1 = a1;
	const struct attribute *attrib2 = a2;

	return attrib1->handle - attrib2->handle;
}

static uint16_t read_value(uint16_t handle, uint8_t *pdu, int len)
{
	struct attribute *a;
	GSList *l;
	guint h = handle;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(h), handle_cmp);
	if (!l)
		return enc_error_resp(ATT_OP_READ_REQ, handle,
					ATT_ECODE_INVALID_HANDLE, pdu, len);

	a = l->data;

	return enc_read_resp(a->data, a->len, pdu, len);
}

static void write_value(uint16_t handle, const uint8_t *value, int vlen)
{
	struct attribute *a;
	GSList *l;
	guint h = handle;
	uuid_t uuid;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(h), handle_cmp);
	if (!l)
		return;

	a = l->data;
	memcpy(&uuid, &a->uuid, sizeof(uuid_t));
	attrib_db_update(handle, &uuid, value, vlen);
}

static uint16_t mtu_exchange(struct gatt_channel *channel, uint16_t mtu,
		uint8_t *pdu, int len)
{
	channel->mtu = MIN(mtu, ATT_MAX_MTU);

	return enc_mtu_resp(channel->mtu, pdu, len);
}

static void channel_disconnect(void *user_data)
{
	struct gatt_channel *channel = user_data;

	g_attrib_unref(channel->attrib);
	clients = g_slist_remove(clients, channel);

	g_free(channel);
}

static void channel_handler(const uint8_t *ipdu, uint16_t len,
							gpointer user_data)
{
	struct gatt_channel *channel = user_data;
	uint8_t opdu[ATT_MAX_MTU], value[ATT_MAX_MTU];
	uint16_t length, start, end, mtu;
	uuid_t uuid;
	uint8_t status = 0;
	int vlen;

	switch(ipdu[0]) {
	case ATT_OP_READ_BY_GROUP_REQ:
		length = dec_read_by_grp_req(ipdu, len, &start, &end, &uuid);
		if (length == 0) {
			status = ATT_ECODE_INVALID_PDU;
			goto done;
		}

		length = read_by_group(start, end, &uuid, opdu, channel->mtu);
		break;
	case ATT_OP_READ_BY_TYPE_REQ:
		length = dec_read_by_type_req(ipdu, len, &start, &end, &uuid);
		if (length == 0) {
			status = ATT_ECODE_INVALID_PDU;
			goto done;
		}

		length = read_by_type(start, end, &uuid, opdu, channel->mtu);
		break;
	case ATT_OP_READ_REQ:
		length = dec_read_req(ipdu, len, &start);
		if (length == 0) {
			status = ATT_ECODE_INVALID_PDU;
			goto done;
		}

		length = read_value(start, opdu, channel->mtu);
		break;
	case ATT_OP_MTU_REQ:
		length = dec_mtu_req(ipdu, len, &mtu);
		if (length == 0) {
			status = ATT_ECODE_INVALID_PDU;
			goto done;
		}

		length = mtu_exchange(channel, mtu, opdu, channel->mtu);
		break;
	case ATT_OP_FIND_INFO_REQ:
		length = dec_find_info_req(ipdu, len, &start, &end);
		if (length == 0) {
			status = ATT_ECODE_INVALID_PDU;
			goto done;
		}

		length = find_info(start, end, opdu, channel->mtu);
		break;
	case ATT_OP_WRITE_CMD:
		length = dec_write_cmd(ipdu, len, &start, value, &vlen);
		if (length > 0)
			write_value(start, value, vlen);
		return;
	case ATT_OP_FIND_BY_TYPE_REQ:
	case ATT_OP_READ_BLOB_REQ:
	case ATT_OP_READ_MULTI_REQ:
	case ATT_OP_WRITE_REQ:
	case ATT_OP_PREP_WRITE_REQ:
	case ATT_OP_EXEC_WRITE_REQ:
	default:
		status = ATT_ECODE_REQ_NOT_SUPP;
		goto done;
	}

	if (length == 0)
		status = ATT_ECODE_IO;

done:
	if (status)
		length = enc_error_resp(ipdu[0], 0x0000, status, opdu, channel->mtu);

	g_attrib_send(channel->attrib, opdu[0], opdu, length,
							NULL, NULL, NULL);
}

static void connect_event(GIOChannel *io, GError *err, void *user_data)
{
	struct gatt_channel *channel;
	GError *gerr = NULL;

	if (err) {
		error("%s", err->message);
		return;
	}

	channel = g_new0(struct gatt_channel, 1);

	bt_io_get(io, BT_IO_L2CAP, &gerr,
			BT_IO_OPT_SOURCE_BDADDR, &channel->src,
			BT_IO_OPT_DEST_BDADDR, &channel->dst,
			BT_IO_OPT_INVALID);
	if (gerr) {
		error("bt_io_get: %s", gerr->message);
		g_error_free(gerr);
		g_free(channel);
		g_io_channel_shutdown(io, TRUE, NULL);
		return;
	}

	channel->attrib = g_attrib_new(io);
	channel->mtu = ATT_DEFAULT_MTU;

	channel->id = g_attrib_register(channel->attrib, GATTRIB_ALL_EVENTS,
				channel_handler, channel, NULL);

	g_attrib_set_disconnect_function(channel->attrib, channel_disconnect,
								channel);

	clients = g_slist_append(clients, channel);
}

static void confirm_event(GIOChannel *io, void *user_data)
{
	GError *gerr = NULL;

	if (bt_io_accept(io, connect_event, NULL, NULL, &gerr) == FALSE) {
		error("bt_io_accept: %s", gerr->message);
		g_error_free(gerr);
		g_io_channel_unref(io);
	}

	return;
}

static gboolean send_notification(gpointer user_data)
{
	uint8_t pdu[ATT_MAX_MTU];
	guint handle = GPOINTER_TO_UINT(user_data);
	struct attribute *a;
	GSList *l;
	uint16_t length;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(handle), handle_cmp);
	if (!l)
		return FALSE;

	a = l->data;

	for (l = clients; l; l = l->next) {
		struct gatt_channel *channel = l->data;

		length = enc_notification(a, pdu, channel->mtu);
		g_attrib_send(channel->attrib, pdu[0], pdu, length, NULL, NULL, NULL);
	}

	return FALSE;
}

static gboolean send_indication(gpointer user_data)
{
	uint8_t pdu[ATT_MAX_MTU];
	guint handle = GPOINTER_TO_UINT(user_data);
	struct attribute *a;
	GSList *l;
	uint16_t length;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(handle), handle_cmp);
	if (!l)
		return FALSE;

	a = l->data;

	for (l = clients; l; l = l->next) {
		struct gatt_channel *channel = l->data;

		length = enc_indication(a, pdu, channel->mtu);
		g_attrib_send(channel->attrib, pdu[0], pdu, length, NULL, NULL, NULL);
	}

	return FALSE;
}

int attrib_server_init(void)
{
	GError *gerr = NULL;
	sdp_record_t *record;

	/* BR/EDR socket */
	l2cap_io = bt_io_listen(BT_IO_L2CAP, NULL, confirm_event,
					NULL, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, BDADDR_ANY,
					BT_IO_OPT_PSM, GATT_PSM,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);

	if (l2cap_io == NULL) {
		error("%s", gerr->message);
		g_error_free(gerr);
		return -1;
	}

	/* LE socket */
	le_io = bt_io_listen(BT_IO_L2CAP, NULL, confirm_event,
					NULL, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, BDADDR_ANY,
					BT_IO_OPT_CID, GATT_CID,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
					BT_IO_OPT_INVALID);

	if (le_io == NULL) {
		error("%s", gerr->message);
		g_error_free(gerr);
		/* Doesn't have LE support, continue */
	}

	record = server_record_new();
	if (record == NULL) {
		error("Unable to create GATT service record");
		return -1;
	}

	if (add_record_to_server(BDADDR_ANY, record) < 0) {
		error("Failed to register GATT service record");
		sdp_record_free(record);
		return -1;
	}

	sdp_handle = record->handle;

	return 0;
}

void attrib_server_exit(void)
{
	GSList *l;

	g_slist_foreach(database, (GFunc) g_free, NULL);
	g_slist_free(database);

	if (l2cap_io) {
		g_io_channel_unref(l2cap_io);
		g_io_channel_shutdown(l2cap_io, FALSE, NULL);
	}

	if (le_io) {
		g_io_channel_unref(le_io);
		g_io_channel_shutdown(le_io, FALSE, NULL);
	}

	for (l = clients; l; l = l->next) {
		struct gatt_channel *channel = l->data;

		g_attrib_unref(channel->attrib);
		g_free(channel);
	}

	g_slist_free(clients);

	if (sdp_handle)
		remove_record_from_server(sdp_handle);
}

int attrib_db_add(uint16_t handle, uuid_t *uuid, const uint8_t *value, int len)
{
	struct attribute *a;

	/* FIXME: handle conflicts */

	a = g_malloc0(sizeof(struct attribute) + len);
	a->handle = handle;
	memcpy(&a->uuid, uuid, sizeof(uuid_t));
	a->len = len;
	memcpy(a->data, value, len);

	database = g_slist_insert_sorted(database, a, attribute_cmp);

	return 0;
}

int attrib_db_update(uint16_t handle, uuid_t *uuid, const uint8_t *value,
								int len)
{
	struct attribute *a;
	GSList *l;
	guint h = handle;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(h), handle_cmp);
	if (!l)
		return -ENOENT;

	a = g_try_realloc(l->data, sizeof(struct attribute) + len);
	if (a == NULL)
		return -ENOMEM;

	l->data = a;
	a->handle = handle;
	memcpy(&a->uuid, uuid, sizeof(uuid_t));
	a->len = len;
	memcpy(a->data, value, len);

	/*
	 * Characteristic configuration descriptor is not being used yet.
	 * If the attribute changes, all connected clients will be notified.
	 * For testing purposes, we send a Notification and a Indication for
	 * each update.
	 */
	g_idle_add(send_notification, GUINT_TO_POINTER(h));

	g_idle_add(send_indication, GUINT_TO_POINTER(h));

	return 0;
}

int attrib_db_del(uint16_t handle)
{
	struct attribute *a;
	GSList *l;
	guint h = handle;

	l = g_slist_find_custom(database, GUINT_TO_POINTER(h), handle_cmp);
	if (!l)
		return -ENOENT;

	a = l->data;
	database = g_slist_remove(database, a);
	g_free(a);

	return 0;
}
