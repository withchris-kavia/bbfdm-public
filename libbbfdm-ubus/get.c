/*
 * get.c: Get handler for bbfdmd
 *
 * Copyright (C) 2023 IOPSYS Software Solutions AB. All rights reserved.
 *
 * Author: Vivek Dutta <vivek.dutta@iopsys.eu>
 * Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 * See LICENSE file for license related information.
 */

#include "common.h"
#include "get_helper.h"

#include <libubus.h>

void bbfdm_get(bbfdm_data_t *data, int method)
{
	struct pathNode *pn = NULL;
	int fault = 0;

	LIST_HEAD(temp_list);

	if (method == BBF_INSTANCES) {
		// referesh reference db
		struct dmctx bbf_ctx = {
			.in_param = ROOT_NODE,
			.dm_type = data->bbf_ctx.dm_type
		};

		bbf_init(&bbf_ctx);
		bbfdm_cmd_exec(&bbf_ctx, BBF_REFERENCES_DB);
		list_splice_tail_init(bbf_ctx.modified_uci_head, &temp_list);
		bbf_cleanup(&bbf_ctx);
	}

	bbf_init(&data->bbf_ctx);

	if (!list_empty(&temp_list)) {
		list_splice_tail_init(&temp_list, data->bbf_ctx.modified_uci_head);
	}

	void *array = blobmsg_open_array(&data->bbf_ctx.bb, "results");

	list_for_each_entry(pn, data->plist, list) {
		bbf_sub_init(&data->bbf_ctx);

		data->bbf_ctx.in_param = pn->path;

		fault = bbfdm_cmd_exec(&data->bbf_ctx, method);
		if (fault) {
			void *table = blobmsg_open_table(&data->bbf_ctx.bb, NULL);
			bb_add_string(&data->bbf_ctx.bb, "path", data->bbf_ctx.in_param);
			blobmsg_add_u32(&data->bbf_ctx.bb, "fault", bbf_fault_map(&data->bbf_ctx, fault));
			bb_add_string(&data->bbf_ctx.bb, "fault_msg", data->bbf_ctx.fault_msg);
			blobmsg_close_table(&data->bbf_ctx.bb, table);
		}

		bbf_sub_cleanup(&data->bbf_ctx);
	}

	blobmsg_close_array(&data->bbf_ctx.bb, array);

	array = blobmsg_open_array(&data->bbf_ctx.bb, "modified_uci");
	if (data->bbf_ctx.modified_uci_head != NULL) {
		struct dm_modified_uci *m;
		list_for_each_entry(m, data->bbf_ctx.modified_uci_head, list) {
			bb_add_string(&data->bbf_ctx.bb, "", m->uci_file);
		}
	}

	blobmsg_close_array(&data->bbf_ctx.bb, array);

	if (!validate_msglen(data)) {
		BBF_ERR("IPC failed for path(%s)", data->bbf_ctx.in_param);
	}

	if (data->ctx && data->req) {
		ubus_send_reply(data->ctx, data->req, data->bbf_ctx.bb.head);
	}

	// Apply all bbfdm dmmap changes
	if (data->bbf_ctx.dm_type == BBFDM_BOTH) {
		dmuci_commit_bbfdm();
	}

	bbf_cleanup(&data->bbf_ctx);
}
