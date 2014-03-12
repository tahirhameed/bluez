/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"
#include "src/sdp-client.h"
#include "src/uuid-helper.h"
#include "src/shared/hfp.h"
#include "btio/btio.h"
#include "hal-msg.h"
#include "ipc-common.h"
#include "ipc.h"
#include "handsfree.h"
#include "bluetooth.h"
#include "src/log.h"
#include "utils.h"

#define HSP_AG_CHANNEL 12
#define HFP_AG_CHANNEL 13

#define HFP_AG_FEAT_3WAY	0x00000001
#define HFP_AG_FEAT_ECNR	0x00000002
#define HFP_AG_FEAT_VR		0x00000004
#define HFP_AG_FEAT_INBAND	0x00000008
#define HFP_AG_FEAT_VTAG	0x00000010
#define HFP_AG_FEAT_REJ_CALL	0x00000020
#define HFP_AG_FEAT_ECS		0x00000040
#define HFP_AG_FEAT_ECC		0x00000080
#define HFP_AG_FEAT_EXT_ERR	0x00000100
#define HFP_AG_FEAT_CODEC	0x00000200

#define HFP_HF_FEAT_ECNR	0x00000001
#define HFP_HF_FEAT_3WAY	0x00000002
#define HFP_HF_FEAT_CLI		0x00000004
#define HFP_HF_FEAT_VR		0x00000008
#define HFP_HF_FEAT_RVC		0x00000010
#define HFP_HF_FEAT_ECS		0x00000020
#define HFP_HF_FEAT_ECC		0x00000040
#define HFP_HF_FEAT_CODEC	0x00000080

#define HFP_AG_FEATURES ( HFP_AG_FEAT_3WAY | HFP_AG_FEAT_ECNR |\
			HFP_AG_FEAT_VR | HFP_AG_FEAT_REJ_CALL |\
			HFP_AG_FEAT_ECS | HFP_AG_FEAT_EXT_ERR )

#define HFP_AG_CHLD "0,1,2,3"

/* offsets in indicators table, should be incremented when sending CIEV */
#define IND_SERVICE	0
#define IND_CALL	1
#define IND_CALLSETUP	2
#define IND_CALLHELD	3
#define IND_SIGNAL	4
#define IND_ROAM	5
#define IND_BATTCHG	6
#define IND_COUNT	(IND_BATTCHG + 1)

#define RING_TIMEOUT 2

struct indicator {
	const char *name;
	int min;
	int max;
	int val;
	bool always_active;
	bool active;
};

static const struct indicator inds_defaults[] = {
		{ "service",   0, 1, 0, false, true },
		{ "call",      0, 1, 0, true, true },
		{ "callsetup", 0, 3, 0, true, true },
		{ "callheld",  0, 2, 0, true, true },
		{ "signal",    0, 5, 0, false, true },
		{ "roam",      0, 1, 0, false, true },
		{ "battchg",   0, 5, 0, false, true },
};

static struct {
	bdaddr_t bdaddr;
	uint8_t state;
	uint8_t audio_state;
	uint32_t features;
	bool clip_enabled;
	bool cmee_enabled;
	bool ccwa_enabled;
	bool indicators_enabled;
	struct indicator inds[IND_COUNT];
	guint ring;
	bool hsp;
	struct hfp_gw *gw;
	GIOChannel *sco;
	guint sco_watch;
} device;

static bdaddr_t adapter_addr;
static struct ipc *hal_ipc = NULL;

static uint32_t hfp_record_id = 0;
static GIOChannel *hfp_server = NULL;

static uint32_t hsp_record_id = 0;
static GIOChannel *hsp_server = NULL;

static GIOChannel *sco_server = NULL;

static void device_set_state(uint8_t state)
{
	struct hal_ev_handsfree_conn_state ev;
	char address[18];

	if (device.state == state)
		return;

	device.state = state;

	ba2str(&device.bdaddr, address);
	DBG("device %s state %u", address, state);

	bdaddr2android(&device.bdaddr, ev.bdaddr);
	ev.state = state;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_EV_HANDSFREE_CONN_STATE, sizeof(ev), &ev);
}

static void device_set_audio_state(uint8_t state)
{
	struct hal_ev_handsfree_audio_state ev;
	char address[18];

	if (device.audio_state == state)
		return;

	device.audio_state = state;

	ba2str(&device.bdaddr, address);
	DBG("device %s audio state %u", address, state);

	bdaddr2android(&device.bdaddr, ev.bdaddr);
	ev.state = state;

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_EV_HANDSFREE_AUDIO_STATE, sizeof(ev), &ev);
}

static void device_init(const bdaddr_t *bdaddr)
{
	bacpy(&device.bdaddr, bdaddr);

	memcpy(device.inds, inds_defaults, sizeof(device.inds));

	device_set_state(HAL_EV_HANDSFREE_CONN_STATE_CONNECTING);
}

static void device_cleanup(void)
{
	if (device.gw) {
		hfp_gw_unref(device.gw);
		device.gw = NULL;
	}

	device_set_state(HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTED);

	if (device.sco_watch) {
		g_source_remove(device.sco_watch);
		device.sco_watch = 0;
	}

	if (device.sco) {
		g_io_channel_shutdown(device.sco, TRUE, NULL);
		g_io_channel_unref(device.sco);
		device.sco = NULL;
	}

	if (device.ring) {
		g_source_remove(device.ring);
		device.ring = 0;
	}

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTED);

	memset(&device, 0, sizeof(device));
}

static void disconnect_watch(void *user_data)
{
	DBG("");

	device_cleanup();
}

static void at_cmd_unknown(const char *command, void *user_data)
{
	uint8_t buf[IPC_MTU];
	struct hal_ev_handsfree_unknown_at *ev = (void *) buf;

	if (device.state != HAL_EV_HANDSFREE_CONN_STATE_SLC_CONNECTED) {
		hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
		hfp_gw_disconnect(device.gw);
		return;
	}

	/* copy while string including terminating NULL */
	ev->len = strlen(command) + 1;
	memcpy(ev->buf, command, ev->len);

	if (ev->len > IPC_MTU - sizeof(*ev)) {
		hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
		return;
	}

	ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
			HAL_EV_HANDSFREE_UNKNOWN_AT, sizeof(*ev) + ev->len, ev);
}

static void at_cmd_vgm(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
								void *user_data)
{
	struct hal_ev_handsfree_volume ev;
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 15)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		ev.type = HAL_HANDSFREE_VOLUME_TYPE_MIC;
		ev.volume = val;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_EV_HANDSFREE_VOLUME, sizeof(ev), &ev);

		/* Framework is not replying with result for AT+VGM */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_vgs(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
								void *user_data)
{
	struct hal_ev_handsfree_volume ev;
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 15)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		ev.type = HAL_HANDSFREE_VOLUME_TYPE_SPEAKER;
		ev.volume = val;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_EV_HANDSFREE_VOLUME, sizeof(ev), &ev);

		/* Framework is not replying with result for AT+VGS */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_cops(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val != 3)
			break;

		if (!hfp_gw_result_get_number(result, &val) || val != 0)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
						HAL_EV_HANDSFREE_COPS, 0, NULL);
		return;
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bia(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val, i, def;
	bool tmp[IND_COUNT];

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		for (i = 0; i < IND_COUNT; i++)
			tmp[i] = device.inds[i].active;

		i = 0;

		do {
			def = (i < IND_COUNT) ? device.inds[i].active : 0;

			if (!hfp_gw_result_get_number_default(result, &val, def))
				goto failed;

			if (val > 1)
				goto failed;

			if (i < IND_COUNT) {
				tmp[i] = val || device.inds[i].always_active;
				i++;
			}
		} while (hfp_gw_result_has_next(result));

		for (i = 0; i < IND_COUNT; i++)
			device.inds[i].active = tmp[i];

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

failed:
	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_a(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_COMMAND:
		if (hfp_gw_result_has_next(result))
			break;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_ANSWER, 0, NULL);

		/* Framework is not replying with result for ATA */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_SET:
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_d(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	char buf[IPC_MTU];
	struct hal_ev_handsfree_dial *ev = (void*) buf;
	int cnt;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_unquoted_string(result,
						(char *) ev->number, 255))
			break;

		ev->number_len = strlen((char *) ev->number);

		if (ev->number[ev->number_len - 1] != ';')
			break;

		if (ev->number[0] == '>')
			cnt = strspn((char *) ev->number + 1, "0123456789") + 1;
		else
			cnt = strspn((char *) ev->number, "0123456789ABC*#+");

		if (cnt != ev->number_len - 1)
			break;

		ev->number_len++;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_DIAL,
					sizeof(*ev) + ev->number_len, ev);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_ccwa(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 1)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		device.ccwa_enabled = val;

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_chup(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_COMMAND:
		if (hfp_gw_result_has_next(result))
			break;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_HANGUP, 0, NULL);

		/* Framework is not replying with result for AT+CHUP */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_SET:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_clcc(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_COMMAND:
		if (hfp_gw_result_has_next(result))
			break;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_CLCC, 0, NULL);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_SET:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_cmee(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 1)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		device.cmee_enabled = val;

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_clip(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 1)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		device.clip_enabled = val;

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_vts(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	struct hal_ev_handsfree_dtmf ev;
	char str[2];

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_unquoted_string(result, str, 2))
			break;

		if (!((str[0] >= '0' && str[0] <='9') ||
				(str[0] >= 'A' && str[0] <= 'D') ||
				str[0] == '*' || str[0] == '#'))
			break;

		if (hfp_gw_result_has_next(result))
			break;

		ev.tone = str[0];

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_DTMF, sizeof(ev), &ev);

		/* Framework is not replying with result for AT+VTS */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_cnum(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_COMMAND:
		if (hfp_gw_result_has_next(result))
			break;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
						HAL_EV_HANDSFREE_CNUM, 0, NULL);

		return;
	case HFP_GW_CMD_TYPE_SET:
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_binp(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bldn(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	struct hal_ev_handsfree_dial ev;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_COMMAND:
		if (hfp_gw_result_has_next(result))
			break;

		ev.number_len = 0;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_DIAL, sizeof(ev), &ev);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_SET:
		break;
	}
}

static void at_cmd_bvra(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	struct hal_ev_handsfree_vr_state ev;
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 1)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		if (val)
			ev.state = HAL_HANDSFREE_VR_STARTED;
		else
			ev.state = HAL_HANDSFREE_VR_STOPPED;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_VR, sizeof(ev), &ev);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_nrec(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	struct hal_ev_handsfree_nrec ev;
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		/* Android HAL defines start and stop parameter for NREC
		 * callback, but spec allows HF to only disable AG's NREC
		 * feature for SLC duration. Follow spec here.
		 */
		if (!hfp_gw_result_get_number(result, &val) || val != 0)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		ev.nrec = HAL_HANDSFREE_NREC_STOP;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_NREC, sizeof(ev), &ev);

		/* Framework is not replying with result for AT+NREC */
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bsir(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_btrh(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bcc(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bcs(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_ckpd(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val != 200)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_EV_HANDSFREE_HSP_KEY_PRESS, 0, NULL);

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void register_post_slc_at(void)
{
	if (device.hsp) {
		hfp_gw_register(device.gw, at_cmd_ckpd, "+CKPD", NULL, NULL);
		hfp_gw_register(device.gw, at_cmd_vgs, "+VGS", NULL, NULL);
		hfp_gw_register(device.gw, at_cmd_vgm, "+VGM", NULL, NULL);
		return;
	}

	hfp_gw_register(device.gw, at_cmd_a, "A", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_d, "D", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_ccwa, "+CCWA", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_chup, "+CHUP", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_clcc, "+CLCC", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_cops, "+COPS", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_cmee, "+CMEE", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_clip, "+CLIP", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_vts, "+VTS", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_cnum, "+CNUM", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bia, "+BIA", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_binp, "+BINP", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bldn, "+BLDN", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bvra, "+BVRA", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_nrec, "+NREC", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_vgs, "+VGS", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_vgm, "+VGM", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bsir, "+BSIR", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_btrh, "+BTRH", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bcc, "+BCC", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bcs, "+BCS", NULL, NULL);
}

static void at_cmd_cmer(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int val;

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		/* mode must be =3 */
		if (!hfp_gw_result_get_number(result, &val) || val != 3)
			break;

		/* keyp is don't care */
		if (!hfp_gw_result_get_number(result, &val))
			break;

		/* disp is don't care */
		if (!hfp_gw_result_get_number(result, &val))
			break;

		/* ind must be 0 or 1 */
		if (!hfp_gw_result_get_number(result, &val) || val > 1)
			break;

		if (hfp_gw_result_has_next(result))
			break;

		device.indicators_enabled = val;

		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		if (device.features & HFP_HF_FEAT_3WAY)
			return;

		register_post_slc_at();
		device_set_state(HAL_EV_HANDSFREE_CONN_STATE_SLC_CONNECTED);

		return;
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_cind(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	char *buf, *ptr;
	int len;
	unsigned int i;

	switch (type) {
	case HFP_GW_CMD_TYPE_TEST:

		len = strlen("+CIND:") + 1;

		for (i = 0; i < IND_COUNT; i++) {
			len += strlen("(\"\",(X,X)),");
			len += strlen(device.inds[i].name);
		}

		buf = g_malloc(len);

		ptr = buf + sprintf(buf, "+CIND:");

		for (i = 0; i < IND_COUNT; i++) {
			ptr += sprintf(ptr, "(\"%s\",(%d%c%d)),",
					device.inds[i].name,
					device.inds[i].min,
					device.inds[i].max == 1 ? ',' : '-',
					device.inds[i].max);
		}

		ptr--;
		*ptr = '\0';

		hfp_gw_send_info(device.gw, "%s", buf);
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		g_free(buf);

		return;
	case HFP_GW_CMD_TYPE_READ:
		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
						HAL_EV_HANDSFREE_CIND, 0, NULL);
		return;
	case HFP_GW_CMD_TYPE_SET:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_brsf(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	unsigned int feat;

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &feat))
			break;

		if (hfp_gw_result_has_next(result))
			break;

		/* TODO verify features */
		device.features = feat;

		hfp_gw_send_info(device.gw, "+BRSF: %u", HFP_AG_FEATURES);
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);
		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_TEST:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_chld(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	struct hal_ev_handsfree_chld ev;
	unsigned int val;

	DBG("");

	switch (type) {
	case HFP_GW_CMD_TYPE_SET:
		if (!hfp_gw_result_get_number(result, &val) || val > 3)
			break;

		/* No ECC support */
		if (hfp_gw_result_has_next(result))
			break;

		/* value match HAL type */
		ev.chld = val;

		ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_EV_HANDSFREE_CHLD, sizeof(ev), &ev);

		return;
	case HFP_GW_CMD_TYPE_TEST:
		hfp_gw_send_info(device.gw, "+CHLD: (%s)", HFP_AG_CHLD);
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		register_post_slc_at();
		device_set_state(HAL_EV_HANDSFREE_CONN_STATE_SLC_CONNECTED);

		return;
	case HFP_GW_CMD_TYPE_READ:
	case HFP_GW_CMD_TYPE_COMMAND:
		break;
	}

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void at_cmd_bac(struct hfp_gw_result *result, enum hfp_gw_cmd_type type,
							void *user_data)
{
	DBG("");

	/* TODO */

	hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);
}

static void register_slc_at(void)
{
	hfp_gw_register(device.gw, at_cmd_brsf, "+BRSF", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_cind, "+CIND", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_cmer, "+CMER", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_chld, "+CHLD", NULL, NULL);
	hfp_gw_register(device.gw, at_cmd_bac, "+BAC", NULL, NULL);
}

static void connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	DBG("");

	if (err) {
		error("handsfree: connect failed (%s)", err->message);
		goto failed;
	}

	device.gw = hfp_gw_new(g_io_channel_unix_get_fd(chan));
	if (!device.gw)
		goto failed;

	g_io_channel_set_close_on_unref(chan, FALSE);

	hfp_gw_set_close_on_unref(device.gw, true);
	hfp_gw_set_command_handler(device.gw, at_cmd_unknown, NULL, NULL);
	hfp_gw_set_disconnect_handler(device.gw, disconnect_watch, NULL, NULL);

	if (device.hsp) {
		register_post_slc_at();
		device_set_state(HAL_EV_HANDSFREE_CONN_STATE_CONNECTED);
		device_set_state(HAL_EV_HANDSFREE_CONN_STATE_SLC_CONNECTED);
		return;
	}

	register_slc_at();
	device_set_state(HAL_EV_HANDSFREE_CONN_STATE_CONNECTED);

	return;

failed:
	g_io_channel_shutdown(chan, TRUE, NULL);
	device_cleanup();
}

static void confirm_cb(GIOChannel *chan, gpointer data)
{
	char address[18];
	bdaddr_t bdaddr;
	GError *err = NULL;

	bt_io_get(chan, &err,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_DEST_BDADDR, &bdaddr,
			BT_IO_OPT_INVALID);
	if (err) {
		error("handsfree: confirm failed (%s)", err->message);
		g_error_free(err);
		goto drop;
	}

	DBG("incoming connect from %s", address);

	if (device.state != HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTED) {
		info("handsfree: refusing connection from %s", address);
		goto drop;
	}

	device_init(&bdaddr);

	if (!bt_io_accept(chan, connect_cb, NULL, NULL, NULL)) {
		error("handsfree: failed to accept connection");
		device_cleanup();
		goto drop;
	}

	device.hsp = GPOINTER_TO_INT(data);

	return;

drop:
	g_io_channel_shutdown(chan, TRUE, NULL);
}

static void sdp_hsp_search_cb(sdp_list_t *recs, int err, gpointer data)
{
	sdp_list_t *protos, *classes;
	GError *gerr = NULL;
	GIOChannel *io;
	uuid_t uuid;
	int channel;

	DBG("");

	if (err < 0) {
		error("handsfree: unable to get SDP record: %s",
								strerror(-err));
		goto fail;
	}

	if (!recs || !recs->data) {
		info("handsfree: no HSP SDP records found");
		goto fail;
	}

	if (sdp_get_service_classes(recs->data, &classes) < 0) {
		error("handsfree: unable to get service classes from record");
		goto fail;
	}

	if (sdp_get_access_protos(recs->data, &protos) < 0) {
		error("handsfree: unable to get access protocols from record");
		sdp_list_free(classes, free);
		goto fail;
	}

	/* TODO read remote version? */
	/* TODO read volume control support */

	memcpy(&uuid, classes->data, sizeof(uuid));
	sdp_list_free(classes, free);

	if (!sdp_uuid128_to_uuid(&uuid) || uuid.type != SDP_UUID16 ||
			uuid.value.uuid16 != HEADSET_SVCLASS_ID) {
		sdp_list_free(protos, NULL);
		error("handsfree: invalid service record or not HSP");
		goto fail;
	}

	channel = sdp_get_proto_port(protos, RFCOMM_UUID);
	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);
	if (channel <= 0) {
		error("handsfree: unable to get RFCOMM channel from record");
		goto fail;
	}

	io = bt_io_connect(connect_cb, NULL, NULL, &gerr,
				BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
				BT_IO_OPT_DEST_BDADDR, &device.bdaddr,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_CHANNEL, channel,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("handsfree: unable to connect: %s", gerr->message);
		g_error_free(gerr);
		goto fail;
	}

	device.hsp = true;

	g_io_channel_unref(io);
	return;

fail:
	device_cleanup();
}

static int sdp_search_hsp(void)
{
	uuid_t uuid;

	sdp_uuid16_create(&uuid, HEADSET_SVCLASS_ID);

	return bt_search_service(&adapter_addr, &device.bdaddr, &uuid,
					sdp_hsp_search_cb, NULL, NULL, 0);
}

static void sdp_hfp_search_cb(sdp_list_t *recs, int err, gpointer data)
{
	sdp_list_t *protos, *classes;
	GError *gerr = NULL;
	GIOChannel *io;
	uuid_t uuid;
	int channel;

	DBG("");

	if (err < 0) {
		error("handsfree: unable to get SDP record: %s",
								strerror(-err));
		goto fail;
	}

	if (!recs || !recs->data) {
		info("handsfree: no HFP SDP records found, trying HSP");

		if (sdp_search_hsp() < 0) {
			error("handsfree: HSP SDP search failed");
			goto fail;
		}

		return;
	}

	if (sdp_get_service_classes(recs->data, &classes) < 0) {
		error("handsfree: unable to get service classes from record");
		goto fail;
	}

	if (sdp_get_access_protos(recs->data, &protos) < 0) {
		error("handsfree: unable to get access protocols from record");
		sdp_list_free(classes, free);
		goto fail;
	}

	/* TODO read remote version? */

	memcpy(&uuid, classes->data, sizeof(uuid));
	sdp_list_free(classes, free);

	if (!sdp_uuid128_to_uuid(&uuid) || uuid.type != SDP_UUID16 ||
			uuid.value.uuid16 != HANDSFREE_SVCLASS_ID) {
		sdp_list_free(protos, NULL);
		error("handsfree: invalid service record or not HFP");
		goto fail;
	}

	channel = sdp_get_proto_port(protos, RFCOMM_UUID);
	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);
	if (channel <= 0) {
		error("handsfree: unable to get RFCOMM channel from record");
		goto fail;
	}

	io = bt_io_connect(connect_cb, NULL, NULL, &gerr,
				BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
				BT_IO_OPT_DEST_BDADDR, &device.bdaddr,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_CHANNEL, channel,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("handsfree: unable to connect: %s", gerr->message);
		g_error_free(gerr);
		goto fail;
	}

	g_io_channel_unref(io);
	return;

fail:
	device_cleanup();
}

static int sdp_search_hfp(void)
{
	uuid_t uuid;

	sdp_uuid16_create(&uuid, HANDSFREE_SVCLASS_ID);

	return bt_search_service(&adapter_addr, &device.bdaddr, &uuid,
					sdp_hfp_search_cb, NULL, NULL, 0);
}

static void handle_connect(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_connect *cmd = buf;
	char addr[18];
	uint8_t status;
	bdaddr_t bdaddr;
	int ret;

	DBG("");

	if (device.state != HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTED) {
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	android2bdaddr(&cmd->bdaddr, &bdaddr);

	ba2str(&bdaddr, addr);
	DBG("connecting to %s", addr);

	device_init(&bdaddr);

	/* prefer HFP over HSP */
	ret = hfp_server ? sdp_search_hfp() : sdp_search_hsp();
	if (ret < 0) {
		error("handsfree: SDP search failed");
		device_cleanup();
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	status = HAL_STATUS_SUCCESS;

failed:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_OP_HANDSFREE_CONNECT, status);
}

static void handle_disconnect(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_disconnect *cmd = buf;
	bdaddr_t bdaddr;
	uint8_t status;

	DBG("");

	android2bdaddr(cmd->bdaddr, &bdaddr);

	if (device.state == HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTED ||
			bacmp(&device.bdaddr, &bdaddr)) {
		status = HAL_STATUS_FAILED;
		goto failed;

	}

	if (device.state == HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTING) {
		status = HAL_STATUS_SUCCESS;
		goto failed;
	}

	if (device.state == HAL_EV_HANDSFREE_CONN_STATE_CONNECTING) {
		device_cleanup();
	} else {
		device_set_state(HAL_EV_HANDSFREE_CONN_STATE_DISCONNECTING);
		hfp_gw_disconnect(device.gw);
	}

	status = HAL_STATUS_SUCCESS;

failed:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_OP_HANDSFREE_DISCONNECT, status);
}

static gboolean sco_watch_cb(GIOChannel *chan, GIOCondition cond,
							gpointer user_data)
{
	g_io_channel_shutdown(device.sco, TRUE, NULL);
	g_io_channel_unref(device.sco);
	device.sco = NULL;

	device.sco_watch = 0;

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTED);

	return FALSE;
}

static void connect_sco_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	if (err) {
		uint8_t status;

		error("SCO: connect failed (%s)", err->message);
		status = HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTED;
		device_set_audio_state(status);

		return;
	}

	g_io_channel_set_close_on_unref(chan, TRUE);

	device.sco = g_io_channel_ref(chan);
	device.sco_watch = g_io_add_watch(chan, G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							sco_watch_cb, NULL);

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_CONNECTED);
}

static bool connect_sco(void)
{
	GIOChannel *io;
	GError *gerr = NULL;

	if (device.sco)
		return false;

	io = bt_io_connect(connect_sco_cb, NULL, NULL, &gerr,
				BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
				BT_IO_OPT_DEST_BDADDR, &device.bdaddr,
				BT_IO_OPT_INVALID);

	if (!io) {
		error("SCO: unable to connect: %s", gerr->message);
		g_error_free(gerr);
		return false;
	}

	g_io_channel_unref(io);

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_CONNECTING);

	return true;
}

static bool disconnect_sco(void)
{
	if (!device.sco)
		return false;

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTING);

	if (device.sco_watch) {
		g_source_remove(device.sco_watch);
		device.sco_watch = 0;
	}

	g_io_channel_shutdown(device.sco, TRUE, NULL);
	g_io_channel_unref(device.sco);
	device.sco = NULL;

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTED);

	return true;
}

static void handle_connect_audio(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_connect_audio *cmd = buf;
	bdaddr_t bdaddr;
	uint8_t status;

	DBG("");

	android2bdaddr(cmd->bdaddr, &bdaddr);

	if (device.audio_state != HAL_EV_HANDSFREE_AUDIO_STATE_DISCONNECTED ||
			bacmp(&device.bdaddr, &bdaddr)) {
		status = HAL_STATUS_FAILED;
		goto done;
	}

	status = connect_sco() ? HAL_STATUS_SUCCESS : HAL_STATUS_FAILED;

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_CONNECT_AUDIO, status);
}

static void handle_disconnect_audio(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_disconnect_audio *cmd = buf;
	bdaddr_t bdaddr;
	uint8_t status;

	DBG("");

	android2bdaddr(cmd->bdaddr, &bdaddr);

	if (device.audio_state != HAL_EV_HANDSFREE_AUDIO_STATE_CONNECTED ||
			bacmp(&device.bdaddr, &bdaddr)) {
		status = HAL_STATUS_FAILED;
		goto done;
	}

	status = disconnect_sco() ? HAL_STATUS_SUCCESS : HAL_STATUS_FAILED;

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_DISCONNECT_AUDIO, status);
}

static void handle_start_vr(const void *buf, uint16_t len)
{
	uint8_t status;

	DBG("");

	if (device.features & HFP_HF_FEAT_VR) {
		hfp_gw_send_info(device.gw, "+BVRA: 1");
		status = HAL_STATUS_SUCCESS;
	} else {
		status = HAL_STATUS_FAILED;
	}

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_OP_HANDSFREE_START_VR, status);
}

static void handle_stop_vr(const void *buf, uint16_t len)
{
	uint8_t status;

	DBG("");

	if (device.features & HFP_HF_FEAT_VR) {
		hfp_gw_send_info(device.gw, "+BVRA: 0");
		status = HAL_STATUS_SUCCESS;
	} else {
		status = HAL_STATUS_FAILED;
	}

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_STOP_VR, status);
}

static void handle_volume_control(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_volume_control *cmd = buf;
	uint8_t status, volume;

	DBG("type=%u volume=%u", cmd->type, cmd->volume);

	volume = cmd->volume > 15 ? 15 : cmd->volume;

	switch (cmd->type) {
	case HAL_HANDSFREE_VOLUME_TYPE_MIC:
		hfp_gw_send_info(device.gw, "+VGM: %u", volume );

		status = HAL_STATUS_SUCCESS;
		break;
	case HAL_HANDSFREE_VOLUME_TYPE_SPEAKER:
		hfp_gw_send_info(device.gw, "+VGS: %u", volume);

		status = HAL_STATUS_SUCCESS;
		break;
	default:
		status = HAL_STATUS_FAILED;
		break;
	}

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_VOLUME_CONTROL, status);
}

static void update_indicator(int ind, uint8_t val)
{
	DBG("ind=%u new=%u old=%u", ind, val, device.inds[ind].val);

	if (device.inds[ind].val == val)
		return;

	device.inds[ind].val = val;

	if (!device.indicators_enabled)
		return;

	if (!device.inds[ind].active)
		return;

	/* indicator numbers in CIEV start from 1 */
	hfp_gw_send_info(device.gw, "+CIEV: %u,%u", ind + 1, val);
}

static void handle_device_status_notif(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_device_status_notif *cmd = buf;

	DBG("");

	update_indicator(IND_SERVICE, cmd->state);
	update_indicator(IND_ROAM, cmd->type);
	update_indicator(IND_SIGNAL, cmd->signal);
	update_indicator(IND_BATTCHG, cmd->battery);

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_OP_HANDSFREE_DEVICE_STATUS_NOTIF,
					HAL_STATUS_SUCCESS);
}

static void handle_cops(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_cops_response *cmd = buf;

	if (len != sizeof(*cmd) + cmd->len ||
			(cmd->len != 0 && cmd->buf[cmd->len - 1] != '\0')) {
		error("Invalid cops response command, terminating");
		raise(SIGTERM);
		return;
	}

	DBG("");

	hfp_gw_send_info(device.gw, "+COPS: 0,0,\"%.16s\"",
					cmd->len ? (char *) cmd->buf : "");

	hfp_gw_send_result(device.gw, HFP_RESULT_OK);

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
			HAL_OP_HANDSFREE_COPS_RESPONSE, HAL_STATUS_SUCCESS);
}

static unsigned int get_callsetup(uint8_t state)
{
	switch (state) {
	case HAL_HANDSFREE_CALL_STATE_INCOMING:
		return 1;
	case HAL_HANDSFREE_CALL_STATE_DIALING:
		return 2;
	case HAL_HANDSFREE_CALL_STATE_ALERTING:
		return 3;
	default:
		return 0;
	}
}

static void handle_cind(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_cind_response *cmd = buf;

	DBG("");

	/* HAL doesn't provide indicators values so need to convert here */
	device.inds[IND_SERVICE].val = cmd->svc;
	device.inds[IND_CALL].val = !!(cmd->num_active + cmd->num_held);
	device.inds[IND_CALLSETUP].val = get_callsetup(cmd->state);
	device.inds[IND_CALLHELD].val = cmd->num_held ?
						(cmd->num_active ? 1 : 2) : 0;
	device.inds[IND_SIGNAL].val = cmd->signal;
	device.inds[IND_ROAM].val = cmd->roam;
	device.inds[IND_BATTCHG].val = cmd->batt_chg;

	/* Order must match indicators_defaults table */
	hfp_gw_send_info(device.gw, "+CIND: %u,%u,%u,%u,%u,%u,%u",
						device.inds[IND_SERVICE].val,
						device.inds[IND_CALL].val,
						device.inds[IND_CALLSETUP].val,
						device.inds[IND_CALLHELD].val,
						device.inds[IND_SIGNAL].val,
						device.inds[IND_ROAM].val,
						device.inds[IND_BATTCHG].val);

	hfp_gw_send_result(device.gw, HFP_RESULT_OK);

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
			HAL_OP_HANDSFREE_CIND_RESPONSE, HAL_STATUS_SUCCESS);
}

static void handle_formatted_at_resp(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_formatted_at_response *cmd = buf;

	DBG("");

	if (len != sizeof(*cmd) + cmd->len ||
			(cmd->len != 0 && cmd->buf[cmd->len - 1] != '\0')) {
		error("Invalid formatted AT response command, terminating");
		raise(SIGTERM);
		return;
	}

	DBG("");

	hfp_gw_send_info(device.gw, "%s", cmd->len ? (char *) cmd->buf : "");

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
					HAL_OP_HANDSFREE_FORMATTED_AT_RESPONSE,
					HAL_STATUS_SUCCESS);
}

static void handle_at_resp(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_at_response *cmd = buf;

	DBG("");

	if (cmd->response == HAL_HANDSFREE_AT_RESPONSE_OK)
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);
	else if (device.cmee_enabled)
		hfp_gw_send_error(device.gw, cmd->error);
	else
		hfp_gw_send_result(device.gw, HFP_RESULT_ERROR);

	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
			HAL_OP_HANDSFREE_AT_RESPONSE, HAL_STATUS_SUCCESS);
}

static void handle_clcc_resp(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_clcc_response *cmd = buf;
	uint8_t status;
	char *number;

	if (len != sizeof(*cmd) + cmd->number_len || (cmd->number_len != 0 &&
				cmd->number[cmd->number_len - 1] != '\0')) {
		error("Invalid CLCC response command, terminating");
		raise(SIGTERM);
		return;
	}

	DBG("");

	if (!cmd->index) {
		hfp_gw_send_result(device.gw, HFP_RESULT_OK);

		status = HAL_STATUS_SUCCESS;
		goto done;
	}

	number = cmd->number_len ? (char *) cmd->number : "";

	switch (cmd->state) {
	case HAL_HANDSFREE_CALL_STATE_INCOMING:
	case HAL_HANDSFREE_CALL_STATE_WAITING:
	case HAL_HANDSFREE_CALL_STATE_ACTIVE:
	case HAL_HANDSFREE_CALL_STATE_HELD:
	case HAL_HANDSFREE_CALL_STATE_DIALING:
	case HAL_HANDSFREE_CALL_STATE_ALERTING:
		if (cmd->type == HAL_HANDSFREE_CALL_ADDRTYPE_INTERNATIONAL &&
							number[0] != '+')
			hfp_gw_send_info(device.gw,
					"+CLCC: %u,%u,%u,%u,%u,\"+%s\",%u",
					cmd->index, cmd->dir, cmd->state,
					cmd->mode, cmd->mpty, number,
					cmd->type);
		else
			hfp_gw_send_info(device.gw,
					"+CLCC: %u,%u,%u,%u,%u,\"%s\",%u",
					cmd->index, cmd->dir, cmd->state,
					cmd->mode, cmd->mpty, number,
					cmd->type);

		status = HAL_STATUS_SUCCESS;
		break;
	case HAL_HANDSFREE_CALL_STATE_IDLE:
	default:
		status = HAL_STATUS_FAILED;
		break;
	}

done:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_CLCC_RESPONSE, status);
}

static gboolean ring_cb(gpointer user_data)
{
	char *clip = user_data;

	hfp_gw_send_info(device.gw, "RING");

	if (device.clip_enabled && clip)
		hfp_gw_send_info(device.gw, "%s", clip);

	return TRUE;
}

static void phone_state_active(int num_active, int num_held)
{

}

static void phone_state_held(int num_active, int num_held)
{

}

static void phone_state_dialing(int num_active, int num_held)
{
	update_indicator(IND_CALLSETUP, 2);
}

static void phone_state_alerting(int num_active, int num_held)
{
	update_indicator(IND_CALLSETUP, 3);
}

static void phone_state_incoming(int num_active, int num_held, uint8_t type,
							const char *number)
{
	char *clip = NULL;

	update_indicator(IND_CALLSETUP, 1);

	if (type == HAL_HANDSFREE_CALL_ADDRTYPE_INTERNATIONAL &&
							number[0] != '+')
		clip = g_strdup_printf("+CLIP: \"+%s\",%u", number, type );
	else
		clip = g_strdup_printf("+CLIP: \"%s\",%u", number, type );

	/* send first RING */
	ring_cb(clip);

	device.ring = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
							RING_TIMEOUT, ring_cb,
							clip, g_free);

	if (!device.ring)
		g_free(clip);
}

static void phone_state_waiting(int num_active, int num_held, uint8_t type,
							const char *number)
{

}

static void phone_state_idle(int num_active, int num_held)
{

	update_indicator(IND_CALL, !!num_active);

	if (device.ring) {
		g_source_remove(device.ring);
		device.ring = 0;
	}

	update_indicator(IND_CALLSETUP, 0);
}

static void handle_phone_state_change(const void *buf, uint16_t len)
{
	const struct hal_cmd_handsfree_phone_state_change *cmd = buf;
	const char *number;
	uint8_t status;

	if (len != sizeof(*cmd) + cmd->number_len || (cmd->number_len != 0 &&
				cmd->number[cmd->number_len - 1] != '\0')) {
		error("Invalid phone state change command, terminating");
		raise(SIGTERM);
		return;
	}

	DBG("active=%u hold=%u state=%u", cmd->num_active, cmd->num_held,
								cmd->state);

	number = cmd->number_len ? (char *) cmd->number : "";

	switch (cmd->state) {
	case HAL_HANDSFREE_CALL_STATE_ACTIVE:
		phone_state_active(cmd->num_active, cmd->num_held);
		break;
	case HAL_HANDSFREE_CALL_STATE_HELD:
		phone_state_held(cmd->num_active, cmd->num_held);
		break;
	case HAL_HANDSFREE_CALL_STATE_DIALING:
		phone_state_dialing(cmd->num_active, cmd->num_held);
		break;
	case HAL_HANDSFREE_CALL_STATE_ALERTING:
		phone_state_alerting(cmd->num_active, cmd->num_held);
		break;
	case HAL_HANDSFREE_CALL_STATE_INCOMING:
		phone_state_incoming(cmd->num_active, cmd->num_held, cmd->type,
									number);
		break;
	case HAL_HANDSFREE_CALL_STATE_WAITING:
		phone_state_waiting(cmd->num_active, cmd->num_held, cmd->type,
									number);
		break;
	case HAL_HANDSFREE_CALL_STATE_IDLE:
		phone_state_idle(cmd->num_active, cmd->num_held);
		break;
	default:
		status = HAL_STATUS_FAILED;
		goto failed;
	}

	status = HAL_STATUS_SUCCESS;

failed:
	ipc_send_rsp(hal_ipc, HAL_SERVICE_ID_HANDSFREE,
				HAL_OP_HANDSFREE_PHONE_STATE_CHANGE, status);
}

static const struct ipc_handler cmd_handlers[] = {
	/* HAL_OP_HANDSFREE_CONNECT */
	{ handle_connect, false, sizeof(struct hal_cmd_handsfree_connect)},
	/* HAL_OP_HANDSFREE_DISCONNECT */
	{handle_disconnect, false, sizeof(struct hal_cmd_handsfree_disconnect)},
	/*HAL_OP_HANDSFREE_CONNECT_AUDIO*/
	{handle_connect_audio, false,
			sizeof(struct hal_cmd_handsfree_connect_audio)},
	/*HAL_OP_HANDSFREE_DISCONNECT_AUDIO*/
	{handle_disconnect_audio, false,
			sizeof(struct hal_cmd_handsfree_disconnect_audio)},
	/* define HAL_OP_HANDSFREE_START_VR */
	{handle_start_vr, false, 0 },
	/* define HAL_OP_HANDSFREE_STOP_VR */
	{handle_stop_vr, false, 0 },
	/* HAL_OP_HANDSFREE_VOLUME_CONTROL */
	{handle_volume_control, false,
			sizeof(struct hal_cmd_handsfree_volume_control)},
	/* HAL_OP_HANDSFREE_DEVICE_STATUS_NOTIF */
	{handle_device_status_notif, false,
			sizeof(struct hal_cmd_handsfree_device_status_notif)},
	/* HAL_OP_HANDSFREE_COPS_RESPONSE */
	{handle_cops, true, sizeof(struct hal_cmd_handsfree_cops_response)},
	/* HAL_OP_HANDSFREE_CIND_RESPONSE */
	{ handle_cind, false, sizeof(struct hal_cmd_handsfree_cind_response)},
	/* HAL_OP_HANDSFREE_FORMATTED_AT_RESPONSE */
	{handle_formatted_at_resp, true,
			sizeof(struct hal_cmd_handsfree_formatted_at_response)},
	/* HAL_OP_HANDSFREE_AT_RESPONSE */
	{handle_at_resp, false, sizeof(struct hal_cmd_handsfree_at_response)},
	/* HAL_OP_HANDSFREE_CLCC_RESPONSE */
	{handle_clcc_resp, true,
			sizeof(struct hal_cmd_handsfree_clcc_response)},
	/* HAL_OP_HANDSFREE_PHONE_STATE_CHANGE */
	{handle_phone_state_change, true,
			sizeof(struct hal_cmd_handsfree_phone_state_change)},
};

static sdp_record_t *headset_ag_record(void)
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, svclass_uuid, ga_svclass_uuid;
	uuid_t l2cap_uuid, rfcomm_uuid;
	sdp_profile_desc_t profile;
	sdp_list_t *aproto, *proto[2];
	sdp_record_t *record;
	sdp_data_t *channel;
	uint8_t netid = 0x01;
	sdp_data_t *network;
	uint8_t ch = HSP_AG_CHANNEL;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	network = sdp_data_alloc(SDP_UINT8, &netid);
	if (!network) {
		sdp_record_free(record);
		return NULL;
	}

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(0, &root_uuid);
	sdp_set_browse_groups(record, root);

	sdp_uuid16_create(&svclass_uuid, HEADSET_AGW_SVCLASS_ID);
	svclass_id = sdp_list_append(0, &svclass_uuid);
	sdp_uuid16_create(&ga_svclass_uuid, GENERIC_AUDIO_SVCLASS_ID);
	svclass_id = sdp_list_append(svclass_id, &ga_svclass_uuid);
	sdp_set_service_classes(record, svclass_id);

	sdp_uuid16_create(&profile.uuid, HEADSET_PROFILE_ID);
	profile.version = 0x0102;
	pfseq = sdp_list_append(0, &profile);
	sdp_set_profile_descs(record, pfseq);

	sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
	proto[0] = sdp_list_append(0, &l2cap_uuid);
	apseq = sdp_list_append(0, proto[0]);

	sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
	proto[1] = sdp_list_append(0, &rfcomm_uuid);
	channel = sdp_data_alloc(SDP_UINT8, &ch);
	proto[1] = sdp_list_append(proto[1], channel);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(0, apseq);
	sdp_set_access_protos(record, aproto);

	sdp_set_info_attr(record, "Voice Gateway", 0, 0);

	sdp_attr_add(record, SDP_ATTR_EXTERNAL_NETWORK, network);

	sdp_data_free(channel);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(pfseq, NULL);
	sdp_list_free(aproto, NULL);
	sdp_list_free(root, NULL);
	sdp_list_free(svclass_id, NULL);

	return record;
}

static void confirm_sco_cb(GIOChannel *chan, gpointer user_data)
{
	char address[18];
	bdaddr_t bdaddr;
	GError *err = NULL;

	if (device.sco)
		goto drop;

	bt_io_get(chan, &err,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_DEST_BDADDR, &bdaddr,
			BT_IO_OPT_INVALID);
	if (err) {
		error("SCO: confirm failed (%s)", err->message);
		g_error_free(err);
		goto drop;
	}

	DBG("incoming SCO connection from %s", address);

	if (device.state != HAL_EV_HANDSFREE_CONN_STATE_SLC_CONNECTED ||
			bacmp(&device.bdaddr, &bdaddr)) {
		error("SCO: connection from %s rejected", address);
		goto drop;
	}

	if (!bt_io_accept(chan, connect_sco_cb, NULL, NULL, NULL)) {
		error("SCO: failed to accept connection");
		goto drop;
	}

	device_set_audio_state(HAL_EV_HANDSFREE_AUDIO_STATE_CONNECTING);

	return;

drop:
	g_io_channel_shutdown(chan, TRUE, NULL);
}

static bool enable_hsp_ag(void)
{
	sdp_record_t *rec;
	GError *err = NULL;

	DBG("");

	hsp_server =  bt_io_listen(NULL, confirm_cb, GINT_TO_POINTER(true), NULL,
					&err,
					BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
					BT_IO_OPT_CHANNEL, HSP_AG_CHANNEL,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_INVALID);
	if (!hsp_server) {
		error("Failed to listen on Headset rfcomm: %s", err->message);
		g_error_free(err);
		return false;
	}

	rec = headset_ag_record();
	if (!rec) {
		error("Failed to allocate Headset record");
		goto failed;
	}

	if (bt_adapter_add_record(rec, 0) < 0) {
		error("Failed to register Headset record");
		sdp_record_free(rec);
		goto failed;
	}

	hsp_record_id = rec->handle;

	return true;

failed:
	g_io_channel_shutdown(hsp_server, TRUE, NULL);
	g_io_channel_unref(hsp_server);
	hsp_server = NULL;

	return false;
}

static void cleanup_hsp_ag(void)
{
	if (hsp_server) {
		g_io_channel_shutdown(hsp_server, TRUE, NULL);
		g_io_channel_unref(hsp_server);
		hsp_server = NULL;
	}

	if (hsp_record_id > 0) {
		bt_adapter_remove_record(hsp_record_id);
		hsp_record_id = 0;
	}
}

static sdp_record_t *hfp_ag_record(void)
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, svclass_uuid, ga_svclass_uuid;
	uuid_t l2cap_uuid, rfcomm_uuid;
	sdp_profile_desc_t profile;
	sdp_list_t *aproto, *proto[2];
	sdp_record_t *record;
	sdp_data_t *channel, *features;
	uint8_t netid = 0x01;
	uint16_t sdpfeat;
	sdp_data_t *network;
	uint8_t ch = HFP_AG_CHANNEL;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	network = sdp_data_alloc(SDP_UINT8, &netid);
	if (!network) {
		sdp_record_free(record);
		return NULL;
	}

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);

	sdp_uuid16_create(&svclass_uuid, HANDSFREE_AGW_SVCLASS_ID);
	svclass_id = sdp_list_append(NULL, &svclass_uuid);
	sdp_uuid16_create(&ga_svclass_uuid, GENERIC_AUDIO_SVCLASS_ID);
	svclass_id = sdp_list_append(svclass_id, &ga_svclass_uuid);
	sdp_set_service_classes(record, svclass_id);

	sdp_uuid16_create(&profile.uuid, HANDSFREE_PROFILE_ID);
	profile.version = 0x0106;
	pfseq = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(record, pfseq);

	sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
	proto[0] = sdp_list_append(0, &l2cap_uuid);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
	proto[1] = sdp_list_append(NULL, &rfcomm_uuid);
	channel = sdp_data_alloc(SDP_UINT8, &ch);
	proto[1] = sdp_list_append(proto[1], channel);
	apseq = sdp_list_append(apseq, proto[1]);

	/* Codec Negotiation bit in SDP feature is different then in BRSF */
	sdpfeat = HFP_AG_FEATURES & 0x0000003F;
	if (HFP_AG_FEATURES & HFP_AG_FEAT_CODEC)
		sdpfeat |= 0x00000020;
	else
		sdpfeat &= ~0x00000020;

	features = sdp_data_alloc(SDP_UINT16, &sdpfeat);
	sdp_attr_add(record, SDP_ATTR_SUPPORTED_FEATURES, features);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);

	sdp_set_info_attr(record, "Hands-Free Audio Gateway", NULL, NULL);

	sdp_attr_add(record, SDP_ATTR_EXTERNAL_NETWORK, network);

	sdp_data_free(channel);
	sdp_list_free(proto[0], NULL);
	sdp_list_free(proto[1], NULL);
	sdp_list_free(apseq, NULL);
	sdp_list_free(pfseq, NULL);
	sdp_list_free(aproto, NULL);
	sdp_list_free(root, NULL);
	sdp_list_free(svclass_id, NULL);

	return record;
}

static bool enable_hfp_ag(void)
{
	sdp_record_t *rec;
	GError *err = NULL;

	DBG("");

	if (hfp_server)
		return false;

	hfp_server =  bt_io_listen(NULL, confirm_cb, GINT_TO_POINTER(false),
					NULL, &err,
					BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
					BT_IO_OPT_CHANNEL, HFP_AG_CHANNEL,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_INVALID);
	if (!hfp_server) {
		error("Failed to listen on Handsfree rfcomm: %s", err->message);
		g_error_free(err);
		return false;
	}

	rec = hfp_ag_record();
	if (!rec) {
		error("Failed to allocate Handsfree record");
		goto failed;
	}

	if (bt_adapter_add_record(rec, 0) < 0) {
		error("Failed to register Handsfree record");
		sdp_record_free(rec);
		goto failed;
	}

	hfp_record_id = rec->handle;

	return true;

failed:
	g_io_channel_shutdown(hfp_server, TRUE, NULL);
	g_io_channel_unref(hfp_server);
	hfp_server = NULL;

	return false;
}

static void cleanup_hfp_ag(void)
{
	if (hfp_server) {
		g_io_channel_shutdown(hfp_server, TRUE, NULL);
		g_io_channel_unref(hfp_server);
		hfp_server = NULL;
	}

	if (hfp_record_id > 0) {
		bt_adapter_remove_record(hfp_record_id);
		hfp_record_id = 0;
	}
}

static bool enable_sco_server(void)
{
	GError *err = NULL;

	sco_server = bt_io_listen(NULL, confirm_sco_cb, NULL, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &adapter_addr,
				BT_IO_OPT_INVALID);
	if (!sco_server) {
		error("Failed to listen on SCO: %s", err->message);
		g_error_free(err);
		cleanup_hsp_ag();
		cleanup_hfp_ag();
		return false;
	}

	return true;
}

static void disable_sco_server(void)
{
	if (sco_server) {
		g_io_channel_shutdown(sco_server, TRUE, NULL);
		g_io_channel_unref(sco_server);
		sco_server = NULL;
	}
}

bool bt_handsfree_register(struct ipc *ipc, const bdaddr_t *addr, uint8_t mode)
{
	DBG("mode 0x%x", mode);

	bacpy(&adapter_addr, addr);

	if (!enable_hsp_ag())
		return false;

	if (mode != HAL_MODE_HANDSFREE_HSP_ONLY && !enable_hfp_ag()) {
		cleanup_hsp_ag();
		return false;
	}

	if (!enable_sco_server()) {
		cleanup_hsp_ag();
		cleanup_hfp_ag();
		return false;
	}

	hal_ipc = ipc;
	ipc_register(hal_ipc, HAL_SERVICE_ID_HANDSFREE, cmd_handlers,
						G_N_ELEMENTS(cmd_handlers));

	return true;
}

void bt_handsfree_unregister(void)
{
	DBG("");

	ipc_unregister(hal_ipc, HAL_SERVICE_ID_HANDSFREE);
	hal_ipc = NULL;

	cleanup_hfp_ag();
	cleanup_hsp_ag();
	disable_sco_server();
}
