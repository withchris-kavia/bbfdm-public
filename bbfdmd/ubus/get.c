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

extern int g_log_level;

static void add_linker_entry(struct async_request_context *ctx, const char *linker_path, const char *linker_value)
{
	struct linker_args *linker = calloc(1, sizeof(struct linker_args));
	if (!linker)
		return;

	list_add_tail(&linker->list, &ctx->linker_list);
	linker->path = strdup(linker_path ? linker_path : "");
	linker->value = strdup(linker_value ? linker_value : "");
}

static void free_linker_entries(struct async_request_context *ctx)
{
	struct linker_args *linker = NULL, *tmp = NULL;

	list_for_each_entry_safe(linker, tmp, &ctx->linker_list, list) {
		list_del(&linker->list);
		BBFDM_FREE(linker->path);
		BBFDM_FREE(linker->value);
		BBFDM_FREE(linker);
	}
}

static bool is_reference_value(struct blob_attr *flags)
{
	struct blob_attr *flag = NULL;
	int rem = 0;

	if (!flags || blobmsg_type(flags) != BLOBMSG_TYPE_ARRAY)
		return false;

	blobmsg_for_each_attr(flag, flags, rem) {
		if (strcmp(blobmsg_get_string(flag), "Reference") == 0)
			return true;
	}

	return false;
}

static void fill_blob_param(struct blob_buf *bb, struct blob_attr *path, const char *data, struct blob_attr *type, struct blob_attr *flags)
{
	if (!bb || !path || !data || !type)
		return;

	void *table = blobmsg_open_table(bb, NULL);

	if (path) {
		blobmsg_add_field(bb, blobmsg_type(path), blobmsg_name(path), blobmsg_data(path), blobmsg_len(path));
	}

	blobmsg_add_string(bb, "data", data);

	if (type) {
		blobmsg_add_field(bb, blobmsg_type(type), blobmsg_name(type), blobmsg_data(type), blobmsg_len(type));
	}

	if (flags) {
		blobmsg_add_field(bb, blobmsg_type(flags), blobmsg_name(flags), blobmsg_data(flags), blobmsg_len(flags));
	}

	blobmsg_close_table(bb, table);
}

static void resolve_reference_path(struct async_request_context *ctx, struct blob_attr *data, char *output, size_t output_len)
{
	if (!ctx || !output || output_len == 0) {
		BBFDM_ERR("Invalid arguments");
		return;
	}

	output[0] = 0; // Ensure output buffer is initialized

	if (!data) {
		BBFDM_ERR("Invalid data value");
		return;
	}

	char *ref_path = blobmsg_get_string(data);
	if (!ref_path || ref_path[0] == '\0') // Empty reference path, nothing to resolve
		return;

	char buffer[MAX_VALUE_LENGTH] = {0};
	snprintf(buffer, sizeof(buffer), "%s", ref_path);

	// Check if it is a reference path (separator ',') or list paths (separator ';')
	bool is_ref_list = strchr(ref_path, ';') != NULL;
	char *token = NULL, *saveptr = NULL;
	unsigned pos = 0;

	for (token = strtok_r(buffer, is_ref_list ? ";" : ",", &saveptr);
		token;
		token = strtok_r(NULL, is_ref_list ? ";" : ",", &saveptr)) {

		// If token does not contain '[', it’s a direct reference
		if (!strchr(token, '[')) {
			pos += snprintf(&output[pos], output_len - pos, "%s,", token);
			if (!is_ref_list) break;
			continue;
		}

		// Search for token in the linker list
		struct linker_args *linker = NULL;
		bool linker_found = false;
		list_for_each_entry(linker, &ctx->linker_list, list) {
			if (strcmp(linker->path, token) == 0 && linker->value[0] != '\0') {
				pos += snprintf(&output[pos], output_len - pos, "%s,", linker->value);
				linker_found = true;
				break;
			}
		}

		if (linker_found) {
			if (!is_ref_list) break;
			continue;
		}

		// If not found, attempt to resolve via micro-services
		{
			// Try to get reference value from micro-services directly
			char *reference_path = get_reference_data(token, "reference_path");

			// Add path to list in order to be used by other parameters
			add_linker_entry(ctx, token, reference_path ? reference_path : "");

			// Reference value is found
			if (reference_path != NULL) {
				pos += snprintf(&output[pos], output_len - pos, "%s,", reference_path);
				BBFDM_FREE(reference_path);
				if (!is_ref_list) break;
			}
		}
	}

	if (pos > 0) {
		output[pos - 1] = 0; // Remove trailing comma
	} else {
		BBFDM_INFO("Can't resolve reference path '%s' -> Set its value to empty", ref_path);
	}
}

static void prepare_and_send_response(struct async_request_context *ctx)
{
	struct blob_attr *attr = NULL;
	struct blob_buf bb = {0};
	int remaining = 0;

	if (!ctx)
		return;

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	void *array = blobmsg_open_array(&bb, "results");

	if (ctx->path_matched == false) {
		void *table = blobmsg_open_table(&bb, NULL);
		blobmsg_add_string(&bb, "path", ctx->requested_path);
		blobmsg_add_u32(&bb, "fault", 9005);
		blobmsg_add_string(&bb, "fault_msg", "Invalid parameter name");
		blobmsg_close_table(&bb, table);
	} else {
		blobmsg_for_each_attr(attr, ctx->tmp_bb.head, remaining) {

			if (strcmp(ctx->ubus_method, "get") == 0) {
				struct blob_attr *fields[4];
				const struct blobmsg_policy policy[4] = {
					{ "path", BLOBMSG_TYPE_STRING },
					{ "data", BLOBMSG_TYPE_STRING },
					{ "type", BLOBMSG_TYPE_STRING },
					{ "flags", BLOBMSG_TYPE_ARRAY },
				};

				blobmsg_parse(policy, 4, fields, blobmsg_data(attr), blobmsg_len(attr));

				if (is_reference_value(fields[3])) {
					char data[MAX_VALUE_LENGTH] = {0};
					resolve_reference_path(ctx, fields[1], data, sizeof(data));
					fill_blob_param(&bb, fields[0], data, fields[2], fields[3]);
				} else {
					blobmsg_add_blob(&bb, attr);
				}
			} else {
				blobmsg_add_blob(&bb, attr);
			}
		}
	}

	blobmsg_close_array(&bb, array);

	ubus_send_reply(ctx->ubus_ctx, &ctx->request_data, bb.head);
	blob_buf_free(&bb);
}

void send_response(struct async_request_context *ctx)
{
	prepare_and_send_response(ctx);

	if (strcmp(ctx->ubus_method, "get") == 0) {
		ubus_unregister_event_handler(ctx->ubus_ctx, &ctx->linker_handler);
		send_linker_cleanup_event(ctx->ubus_ctx);
		free_linker_entries(ctx);
	}

	ubus_complete_deferred_request(ctx->ubus_ctx, &ctx->request_data, UBUS_STATUS_OK);
	blob_buf_free(&ctx->tmp_bb);
	BBFDM_FREE(ctx);
}

static struct blob_attr *get_results_array(struct blob_attr *msg)
{
	struct blob_attr *tb[1] = {0};
	const struct blobmsg_policy p[1] = {
			{ "results", BLOBMSG_TYPE_ARRAY }
	};

	if (msg == NULL)
		return NULL;

	blobmsg_parse(p, 1, tb, blobmsg_data(msg), blobmsg_len(msg));

	return tb[0];
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

void send_linker_cleanup_event(struct ubus_context *ctx)
{
	struct blob_buf bb = {0};

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);
	ubus_send_event(ctx, "bbfdm.linker.cleanup", bb.head);
	blob_buf_free(&bb);
}

void linker_response_callback(struct ubus_context *ctx __attribute__((unused)), struct ubus_event_handler *ev, const char *type __attribute__((unused)), struct blob_attr *msg)
{
	struct async_request_context *context = NULL;
	struct blob_attr *attr = NULL;
	size_t rem = 0;

	if (!msg)
		return;

	context = container_of(ev, struct async_request_context, linker_handler);
	if (context == NULL) {
		BBFDM_ERR("Failed to get the request context");
		return;
	}

	blobmsg_for_each_attr(attr, msg, rem) {
		BBFDM_DEBUG("LINKER RESPONSE: '%s' <=> '%s'", blobmsg_name(attr), blobmsg_get_string(attr));
		add_linker_entry(context, blobmsg_name(attr), blobmsg_get_string(attr));
	}
}
