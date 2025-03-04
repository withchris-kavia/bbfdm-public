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

#include <string.h>
#include <libubox/blobmsg_json.h>

#include "common.h"

int g_log_level = LOG_ERR;

unsigned int get_proto_type(const char *proto)
{
	int type = BBFDMD_BOTH;

	if (proto) {
		if (strcmp(proto, "cwmp") == 0)
			type = BBFDMD_CWMP;
		else if (strcmp(proto, "usp") == 0)
			type = BBFDMD_USP;
		else
			type = BBFDMD_BOTH;
	}

	return type;
}

static bool is_raw_format_type(const char *format)
{
	bool raw_format = false;

	if (format) {
		if (strcmp(format, "raw") == 0)
			raw_format = true;
		else
			raw_format = false;
	}

	return raw_format;
}

void fill_optional_input(struct blob_attr *msg, unsigned int *proto, bool *raw_format)
{
	struct blob_attr *tb[2] = {0};
	const struct blobmsg_policy p[2] = {
			{ "proto", BLOBMSG_TYPE_STRING },
			{ "format", BLOBMSG_TYPE_STRING }
	};

	*proto = BBFDMD_BOTH;
	*raw_format = false;

	if (!msg)
		return;

	blobmsg_parse(p, 2, tb, blobmsg_data(msg), blobmsg_len(msg));

	if (tb[0]) {
		const char *val = blobmsg_get_string(tb[0]);
		*proto = get_proto_type(val);
	}

	if (tb[1]) {
		const char *val = blobmsg_get_string(tb[1]);
		*raw_format = is_raw_format_type(val);
	}
}

struct blob_attr *get_results_array(struct blob_attr *msg)
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

bool proto_matches(unsigned int dm_type, const enum bbfdmd_type_enum type)
{
	return (dm_type == BBFDMD_BOTH || type == BBFDMD_BOTH || dm_type == type) && type != BBFDMD_NONE;
}

static void sync_callback(struct ubus_request *req, int type __attribute__((unused)), struct blob_attr *msg)
{
	struct blob_attr *attr = NULL;
	int remaining = 0;

	if (!req || !msg)
		return;

	struct blob_buf *bb_response = (struct blob_buf *)req->priv;

	if (!bb_response)
		return;

	blob_for_each_attr(attr, msg, remaining) {
		blobmsg_add_field(bb_response, blobmsg_type(attr), blobmsg_name(attr), blobmsg_data(attr), blobmsg_len(attr));
	}
}

static void generate_reference_to_set(const char *in_value, char *output_str, size_t output_str_len)
{
	char token_buffer[MAX_VALUE_LENGTH] = {0};
	char *token = NULL, *saveptr = NULL;
	unsigned pos = 0;

	if (!output_str || output_str_len == 0)
		return;

	output_str[0] = '\0'; // Ensure output buffer is initialized

	if (!in_value || in_value[0] == '\0') // Empty value, nothing to make
		return;

	bbfdm_strncpy(token_buffer, in_value, sizeof(token_buffer));

	for (token = strtok_r(token_buffer, ",", &saveptr); token; token = strtok_r(NULL, ",", &saveptr)) {
		char *reference_value = get_reference_data(token, "reference_value");
		pos += snprintf(&output_str[pos], output_str_len - pos, "%s=>%s##,", token, reference_value ? reference_value : "");
		BBFDM_FREE(reference_value);
	}

	if (pos > 0) {
		output_str[pos - 1] = 0; // Remove trailing comma
	}
}

void run_sync_call(const char *ubus_obj, const char *ubus_method, struct blob_attr *msg, struct blob_buf *bb_response)
{
	struct blob_buf req_buf = {0};
	struct blob_attr *attr = NULL;
	int remaining = 0;

	if (!ubus_obj || !ubus_method || !msg || !bb_response)
		return;

	memset(&req_buf, 0, sizeof(struct blob_buf));
	blob_buf_init(&req_buf, 0);

	blob_for_each_attr(attr, msg, remaining) {
		if (strcmp(ubus_method, "set") == 0 &&
				strcmp(blobmsg_name(attr), "value") == 0 &&
				blobmsg_type(attr) == BLOBMSG_TYPE_STRING &&
				strncmp(BBFDM_ROOT_OBJECT, blobmsg_get_string(attr), strlen(BBFDM_ROOT_OBJECT)) == 0) {
			char reference_to_set[MAX_VALUE_LENGTH] = {0};

			generate_reference_to_set(blobmsg_get_string(attr), reference_to_set, sizeof(reference_to_set));
			blobmsg_add_string(&req_buf, blobmsg_name(attr), reference_to_set);
		} if (strcmp(ubus_method, "set") == 0 &&
				strcmp(blobmsg_name(attr), "obj_path") == 0 &&
				blobmsg_type(attr) == BLOBMSG_TYPE_TABLE) {
			struct blob_attr *__attr = NULL;
			int rem = 0;

			void *table = blobmsg_open_table(&req_buf, "obj_path");

			blobmsg_for_each_attr(__attr, attr, rem) {
				if (blobmsg_type(__attr) == BLOBMSG_TYPE_STRING && strncmp(BBFDM_ROOT_OBJECT, blobmsg_get_string(__attr), strlen(BBFDM_ROOT_OBJECT)) == 0) {
					char reference_to_set[MAX_VALUE_LENGTH] = {0};

					generate_reference_to_set(blobmsg_get_string(__attr), reference_to_set, sizeof(reference_to_set));
					blobmsg_add_string(&req_buf, blobmsg_name(__attr), reference_to_set);
				} else {
					blobmsg_add_string(&req_buf, blobmsg_name(__attr), blobmsg_get_string(__attr));
				}
			}

			blobmsg_close_table(&req_buf, table);
		} else {
			blobmsg_add_field(&req_buf, blobmsg_type(attr), blobmsg_name(attr), blobmsg_data(attr), blobmsg_len(attr));
		}
	}

	if (g_log_level == LOG_DEBUG) {
		char *json_str = blobmsg_format_json_indent(req_buf.head, true, -1);
		BBFDM_DEBUG("### ubus call %s %s '%s' ###", ubus_obj, ubus_method, json_str);
		BBFDM_FREE(json_str);		
	}

	BBFDM_UBUS_INVOKE_SYNC(ubus_obj, ubus_method, req_buf.head, 5000, sync_callback, bb_response);

	blob_buf_free(&req_buf);
}
