/*
 * Copyright (C) 2025 iopsys Software Solutions AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 *	  Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 */

#include <libubus.h>
#include <libubox/blobmsg_json.h>

#include "common.h"
#include "service.h"
#include "get.h"
#include "pretty_print.h"

extern int g_log_level;

static void prepare_and_send_response(struct async_request_context *ctx)
{
	struct blob_attr *attr = NULL;
	struct blob_buf bb_raw = {0};
	size_t remaining = 0;

	if (!ctx)
		return;

	memset(&bb_raw, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb_raw, 0);

	void *array = blobmsg_open_array(&bb_raw, "results");

	if (ctx->path_matched == false) {
		void *table = blobmsg_open_table(&bb_raw, NULL);
		blobmsg_add_string(&bb_raw, "path", ctx->requested_path);
		blobmsg_add_u32(&bb_raw, "fault", 9005);
		blobmsg_add_string(&bb_raw, "fault_msg", "Invalid parameter name");
		blobmsg_close_table(&bb_raw, table);
	} else {
		blobmsg_for_each_attr(attr, ctx->tmp_bb.head, remaining) {
			blobmsg_add_blob(&bb_raw, attr);
		}
	}

	blobmsg_close_array(&bb_raw, array);

	if (strcmp(ctx->ubus_method, "get") == 0 && ctx->raw_format == false) { // Pretty Format
		struct blob_buf bb_pretty = {0};

		memset(&bb_pretty, 0, sizeof(struct blob_buf));
		blob_buf_init(&bb_pretty, 0);

		prepare_pretty_response(ctx->requested_path, bb_raw.head, &bb_pretty);

		ubus_send_reply(ctx->ubus_ctx, &ctx->request_data, bb_pretty.head);
		blob_buf_free(&bb_pretty);
	} else { // Raw Format
		ubus_send_reply(ctx->ubus_ctx, &ctx->request_data, bb_raw.head);
	}

	blob_buf_free(&bb_raw);
}

void send_response(struct async_request_context *ctx)
{
	prepare_and_send_response(ctx);

	ubus_complete_deferred_request(ctx->ubus_ctx, &ctx->request_data, UBUS_STATUS_OK);
	blob_buf_free(&ctx->tmp_bb);
	BBFDM_FREE(ctx);
}

static void append_response_data(struct ubus_request_tracker *tracker, struct blob_attr *msg)
{
	struct blob_attr *attr = NULL;
	int remaining = 0;

	if (!tracker || !msg)
		return;

	struct blob_attr *results = get_results_array(msg);
	if (!results)
		return;

	blobmsg_for_each_attr(attr, results, remaining) {
		blobmsg_add_blob(&tracker->ctx->tmp_bb, attr);
	}
}

static void handle_request_timeout(struct uloop_timeout *timeout)
{
	struct ubus_request_tracker *tracker = container_of(timeout, struct ubus_request_tracker, timeout);
	BBFDM_ERR("Timeout occurred for request: '%s'", tracker->request_name);

	ubus_abort_request(tracker->ctx->ubus_ctx, &tracker->async_request);
	tracker->ctx->pending_requests--;

	if (tracker->ctx->pending_requests == 0 && tracker->ctx->service_list_processed) {
		BBFDM_ERR("All requests completed after timeout");
		send_response(tracker->ctx);
	}

	BBFDM_FREE(tracker);
}

static void ubus_result_callback(struct ubus_request *req, int type __attribute__((unused)), struct blob_attr *msg __attribute__((unused)))
{
	struct ubus_request_tracker *tracker = container_of(req, struct ubus_request_tracker, async_request);

	if (msg) {
		BBFDM_DEBUG("Response from object '%s'", tracker->request_name);
		append_response_data(tracker, msg);
	}
}

static void ubus_request_complete(struct ubus_request *req, int ret)
{
	struct ubus_request_tracker *tracker = container_of(req, struct ubus_request_tracker, async_request);
	BBFDM_DEBUG("Request completed for '%s' with status: '%d'", tracker->request_name, ret);

	uloop_timeout_cancel(&tracker->timeout);
	tracker->ctx->pending_requests--;

	if (tracker->ctx->pending_requests == 0 && tracker->ctx->service_list_processed) {
		BBFDM_DEBUG("Result Callback: All requests completed");
		send_response(tracker->ctx);
	}

	BBFDM_FREE(tracker);
}

void run_async_call(struct async_request_context *ctx, const char *ubus_obj, struct blob_attr *msg)
{
	struct blob_buf req_buf = {0};
	struct blob_attr *attr = NULL;
	int remaining = 0;
	uint32_t id = 0;

	if (!ctx || !ubus_obj || !msg) {
		BBFDM_ERR("Invalid arguments");
		return;
	}

	if (ubus_lookup_id(ctx->ubus_ctx, ubus_obj, &id)) {
		BBFDM_ERR("Failed to lookup object: %s", ubus_obj);
		return;
	}

	struct ubus_request_tracker *tracker = calloc(1, sizeof(struct ubus_request_tracker));
	if (!tracker) {
		BBFDM_ERR("Failed to allocate memory for request tracker");
		return;
	}

	tracker->ctx = ctx;
	ctx->pending_requests++;
	ctx->path_matched = true;

	memset(&req_buf, 0, sizeof(struct blob_buf));
	blob_buf_init(&req_buf, 0);

	blob_for_each_attr(attr, msg, remaining) {
		blobmsg_add_field(&req_buf, blobmsg_type(attr), blobmsg_name(attr), blobmsg_data(attr), blobmsg_len(attr));
	}

	snprintf(tracker->request_name, sizeof(tracker->request_name), "%s->%s", ubus_obj, ctx->ubus_method);

	tracker->timeout.cb = handle_request_timeout;
	uloop_timeout_set(&tracker->timeout, !strcmp(ctx->ubus_method, "operate") ? SERVICE_CALL_OPERATE_TIMEOUT : SERVICE_CALL_TIMEOUT);

	if (g_log_level == LOG_DEBUG) {
		char *json_str = blobmsg_format_json_indent(req_buf.head, true, -1);
		BBFDM_DEBUG("### ubus call %s %s '%s' ###", ubus_obj, ctx->ubus_method, json_str);
		BBFDM_FREE(json_str);
	}

	if (ubus_invoke_async(ctx->ubus_ctx, id, ctx->ubus_method, req_buf.head, &tracker->async_request)) {
		BBFDM_ERR("Failed to invoke async method for object: %s", tracker->request_name);
		uloop_timeout_cancel(&tracker->timeout);
		BBFDM_FREE(tracker);
	} else {
		tracker->async_request.data_cb = ubus_result_callback;
		tracker->async_request.complete_cb = ubus_request_complete;
		ubus_complete_request_async(ctx->ubus_ctx, &tracker->async_request);
	}

	blob_buf_free(&req_buf);
}
