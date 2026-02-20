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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <dirent.h>
#include <json-c/json.h>
#include <libubox/blobmsg_json.h>

#include "common.h"
#include "service.h"

LIST_HEAD(registered_services);

static void add_service_to_list(const char *name, struct blob_buf *dm_schema, int service_proto, int service_timeout,
		service_object_t *objects, size_t count, bool is_unified)
{
	service_entry_t *service = NULL;

	if (!name || !objects || count == 0) {
		BBFDM_ERR("Invalid service registration parameters");
		return;
	}

	service = (service_entry_t *)calloc(1, sizeof(service_entry_t));
	if (!service) {
		BBFDM_ERR("Failed to allocate memory");
		return;
	}

	list_add_tail(&service->list, &registered_services);

	service->name = strdup(name);
	service->dm_schema = dm_schema;
	service->protocol = service_proto;
	service->timeout = service_timeout;
	service->objects = objects;
	service->object_count = count;
	service->is_unified = is_unified;
}

static void receive_schema_result(struct ubus_request *req, int type __attribute__((unused)), struct blob_attr *msg)
{
	struct blob_attr *attr = NULL;
	int remaining = 0;

	if (msg == NULL || req == NULL)
		return;

	struct blob_buf *srv_schema = (struct blob_buf *)req->priv;
	if (!srv_schema)
		return;

	struct blob_attr *results = get_results_array(msg);
	if (!results)
		return;

	blobmsg_for_each_attr(attr, results, remaining) {
		blobmsg_add_blob(srv_schema, attr);
	}
}

void fill_service_schema(struct ubus_context *ubus_ctx, int ubus_timeout, const char *service_name, struct blob_buf **service_schema)
{
	uint32_t ubus_id;

	if (!ubus_ctx || !service_name || !service_schema)
		return;

	if (*service_schema != NULL) {
		blob_buf_free(*service_schema);
		BBFDM_FREE(*service_schema);
	}

	if (!ubus_lookup_id(ubus_ctx, service_name, &ubus_id)) {
		struct blob_buf bb = {0};

		*service_schema = (struct blob_buf *)calloc(1, sizeof(struct blob_buf));
		if (*service_schema == NULL) {
			BBFDM_ERR("Failed to allocate memory");
			return;
		}

		blob_buf_init(*service_schema, 0);

		memset(&bb, 0, sizeof(struct blob_buf));
		blob_buf_init(&bb, 0);

		blobmsg_add_string(&bb, "path", BBFDM_ROOT_OBJECT);

		void *table = blobmsg_open_table(&bb, "optional");
		blobmsg_add_string(&bb, "proto", "usp");
		blobmsg_close_table(&bb, table);

		int err = ubus_invoke(ubus_ctx, ubus_id, "schema", bb.head, receive_schema_result, (void *)*service_schema, ubus_timeout);

		if (err != 0) {
			BBFDM_ERR("UBUS invoke failed [object: %s, method: schema] with error (%d)", service_name, err);
		}

		blob_buf_free(&bb);
	} else {
		BBFDM_WARNING("Failed to lookup UBUS object: %s", service_name);
	}
}

static int load_service_from_file(struct ubus_context *ubus_ctx, const char *filename, const char *file_path)
{
	size_t num_objs = 0;

	if (!filename || !file_path) {
		BBFDM_ERR("Invalid filename or file path");
		return -1;
	}

	json_object *json_root = json_object_from_file(file_path);
	if (!json_root) {
		BBFDM_ERR("Failed to read JSON file: %s", file_path);
		return -1;
	}

	json_object *daemon_config = NULL;
	json_object_object_get_ex(json_root, "daemon", &daemon_config);
	if (!daemon_config) {
		BBFDM_ERR("Failed to find daemon object");
		json_object_put(json_root);
		return -1;
	}

	json_object *enable_jobj = NULL;
	json_object_object_get_ex(daemon_config, "enable", &enable_jobj);
	bool enable = enable_jobj ? json_object_get_boolean(enable_jobj) : false;
	if (!enable) {
		BBFDM_INFO("Service is disabled, Skipping service");
		json_object_put(json_root);
		return -1;
	}

	struct blob_buf *service_schema = NULL;
	char service_name[MAX_PATH_LENGTH] = {0};

	snprintf(service_name, sizeof(service_name), "%s.%.*s", BBFDM_UBUS_OBJECT, (int)(strlen(filename) - 5), filename);
	fill_service_schema(ubus_ctx, 2000, service_name, &service_schema);

	json_object *unified_daemon_jobj = NULL;
	json_object_object_get_ex(daemon_config, "unified_daemon", &unified_daemon_jobj);
	bool is_unified = unified_daemon_jobj ? json_object_get_boolean(unified_daemon_jobj) : false;

	json_object *proto_jobj = NULL;
	json_object_object_get_ex(daemon_config, "proto", &proto_jobj);
	int service_proto = get_proto_type(proto_jobj ? json_object_get_string(proto_jobj) : "");

	json_object *timeout_jobj = NULL;
	json_object_object_get_ex(daemon_config, "timeout", &timeout_jobj);
	int service_timeout = timeout_jobj ? json_object_get_int(timeout_jobj) : SERVICE_CALL_TIMEOUT;

	json_object *services_array = NULL;
	if (!json_object_object_get_ex(daemon_config, "services", &services_array) || json_object_get_type(services_array) != json_type_array) {
		json_object_put(json_root);
		return -1;
	}

	size_t service_count = json_object_array_length(services_array);
	if (service_count == 0) {
		BBFDM_WARNING("Skipping service '%s' due to no objects defined", service_name);
		json_object_put(json_root);
		return -1;
	}

	service_object_t *objects = (service_object_t *)calloc(service_count, sizeof(service_object_t));
	if (!objects) {
		BBFDM_ERR("Failed to allocate memory");
		json_object_put(json_root);
		return -1;
	}

	for (size_t i = 0; i < service_count; i++) {
		json_object *service_obj = json_object_array_get_idx(services_array, i);
		json_object *parent_dm = NULL, *object = NULL, *proto = NULL;

		json_object_object_get_ex(service_obj, "parent_dm", &parent_dm);
		json_object_object_get_ex(service_obj, "object", &object);
		json_object_object_get_ex(service_obj, "proto", &proto);

		snprintf(objects[num_objs].parent_path, sizeof(objects[num_objs].parent_path), "%s", parent_dm ? json_object_get_string(parent_dm) : "");
		snprintf(objects[num_objs].object_name, sizeof(objects[num_objs].object_name), "%s", object ? json_object_get_string(object) : "");

		if (strlen(objects[num_objs].parent_path) == 0 || strlen(objects[num_objs].object_name) == 0) {
			BBFDM_WARNING("Skip empty registration parent_dm[%s] or object[%s]", objects[num_objs].parent_path, objects[num_objs].object_name);
			continue;
		}

		objects[num_objs].protocol = get_proto_type(proto ? json_object_get_string(proto) : "");
		num_objs++;
	}

	BBFDM_INFO("Registering [%s :: %lu :: %d]", service_name, num_objs, is_unified);
	add_service_to_list(service_name, service_schema, service_proto, service_timeout, objects, num_objs, is_unified);
	json_object_put(json_root);
	return 0;
}

static int filter(const struct dirent *entry)
{
	return entry->d_name[0] != '.';
}

static int compare(const struct dirent **a, const struct dirent **b)
{
	size_t len_a = strlen((*a)->d_name);
	size_t len_b = strlen((*b)->d_name);

	if (len_a < len_b) // Sort by length (shorter first)
		return -1;

	if (len_a > len_b)
		return 1;

	return strcasecmp((*a)->d_name, (*b)->d_name); // If lengths are equal, sort alphabetically
}

typedef struct {
	char **paths;
	char **filenames;
	size_t count;
	size_t capacity;
} file_list_t;

static int file_list_init(file_list_t *fl, size_t capacity)
{
	fl->paths = calloc(capacity, sizeof(char *));
	fl->filenames = calloc(capacity, sizeof(char *));
	if (!fl->paths || !fl->filenames) {
		BBFDM_FREE(fl->paths);
		BBFDM_FREE(fl->filenames);
		return -1;
	}

	fl->count = 0;
	fl->capacity = capacity;
	return 0;
}

static int file_list_push(file_list_t *fl, const char *file_path, const char *filename)
{
	if (fl->count >= fl->capacity) {
		BBFDM_ERR("file_list_push: capacity exhausted (%zu)", fl->capacity);
		return -1;
	}

	fl->paths[fl->count] = strdup(file_path);
	fl->filenames[fl->count] = strdup(filename);
	if (!fl->paths[fl->count] || !fl->filenames[fl->count]) {
		BBFDM_FREE(fl->paths[fl->count]);
		BBFDM_FREE(fl->filenames[fl->count]);
		return -1;
	}

	fl->count++;
	return 0;
}

static void file_list_free(file_list_t *fl)
{
	for (size_t i = 0; i < fl->count; i++) {
		BBFDM_FREE(fl->paths[i]);
		BBFDM_FREE(fl->filenames[i]);
	}

	BBFDM_FREE(fl->paths);
	BBFDM_FREE(fl->filenames);
	memset(fl, 0, sizeof(*fl));
}

static bool peek_is_unified(const char *file_path)
{
	json_object *json_root = json_object_from_file(file_path);
	if (!json_root)
		return false;

	json_object *daemon_config = NULL;
	json_object_object_get_ex(json_root, "daemon", &daemon_config);
	if (!daemon_config) {
		json_object_put(json_root);
		return false;
	}

	json_object *unified_jobj = NULL;
	json_object_object_get_ex(daemon_config, "unified_daemon", &unified_jobj);
	bool is_unified = unified_jobj ? json_object_get_boolean(unified_jobj) : false;

	json_object_put(json_root);
	return is_unified;
}

static void fill_from_services(json_object *services_arr, const char *source_name, service_object_t *objects,
				size_t *num_objs, size_t total_capacity)
{
	size_t _len = json_object_array_length(services_arr);
	for (size_t _j = 0; _j < _len; _j++) {
		json_object *_svc  = json_object_array_get_idx(services_arr, _j);
		json_object *_pdm  = NULL, *_obj = NULL, *_proto = NULL;
		const char *_pdm_str, *_obj_str;

		json_object_object_get_ex(_svc, "parent_dm", &_pdm);
		json_object_object_get_ex(_svc, "object", &_obj);
		json_object_object_get_ex(_svc, "proto", &_proto);

		_pdm_str = _pdm ? json_object_get_string(_pdm) : "";
		_obj_str = _obj ? json_object_get_string(_obj) : "";

		memset(objects[*num_objs].parent_path, 0, sizeof(objects[*num_objs].parent_path));
		memset(objects[*num_objs].object_name, 0, sizeof(objects[*num_objs].object_name));

		strncpy(objects[*num_objs].parent_path, _pdm_str, sizeof(objects[*num_objs].parent_path) - 1);
		strncpy(objects[*num_objs].object_name, _obj_str, sizeof(objects[*num_objs].object_name) - 1);
		if (!strlen(objects[*num_objs].parent_path) || !strlen(objects[*num_objs].object_name)) {
			BBFDM_WARNING("Skip empty parent_dm/object in %s", source_name);
			continue;
		}

		objects[*num_objs].protocol = get_proto_type(_proto ? json_object_get_string(_proto) : "");

		(*num_objs)++;
		if (*num_objs >= total_capacity) {
			BBFDM_WARNING("Reached object capacity in %s", source_name);
			break;
		}
	}
}

static void load_non_unified_services(struct ubus_context *ubus_ctx, file_list_t *fl)
{
	if (fl->count == 0)
		return;

	/* Locate core.json in the list and move it to index 0 */
	size_t core_idx = fl->count;
	for (size_t i = 0; i < fl->count; i++) {
		if (strcmp(fl->filenames[i], "core.json") == 0) {
			core_idx = i;
			break;
		}
	}

	if (core_idx == fl->count) {
		BBFDM_ERR("core.json not found in non-unified service list – aborting");
		return;
	}

	/* Swap core.json to position 0 so the loop below handles it first. */
	if (core_idx != 0) {
		char *tmp_path = fl->paths[0];
		fl->paths[0] = fl->paths[core_idx];
		fl->paths[core_idx] = tmp_path;

		char *tmp_name = fl->filenames[0];
		fl->filenames[0] = fl->filenames[core_idx];
		fl->filenames[core_idx] = tmp_name;
	}

	/* Load core.json – derive the shared service configuration */
	json_object *core_root = json_object_from_file(fl->paths[0]);
	if (!core_root) {
		BBFDM_ERR("Failed to read core.json: %s", fl->paths[0]);
		return;
	}

	json_object *core_daemon = NULL;
	json_object_object_get_ex(core_root, "daemon", &core_daemon);
	if (!core_daemon) {
		BBFDM_ERR("core.json missing 'daemon' object");
		json_object_put(core_root);
		return;
	}

	json_object *enable_jobj = NULL;
	json_object_object_get_ex(core_daemon, "enable", &enable_jobj);
	bool enable = enable_jobj ? json_object_get_boolean(enable_jobj) : false;
	if (!enable) {
		BBFDM_INFO("core service is disabled, Skipping service");
		json_object_put(core_root);
		return;
	}

	/* Service name is derived from core.json's filename. */
	char service_name[MAX_PATH_LENGTH] = {0};
	const char *core_fname = fl->filenames[0];
	snprintf(service_name, sizeof(service_name), "%s.%.*s", BBFDM_UBUS_OBJECT, (int)(strlen(core_fname) - 5), core_fname);

	struct blob_buf *service_schema = NULL;
	fill_service_schema(ubus_ctx, 2000, service_name, &service_schema);

	json_object *proto_jobj = NULL;
	json_object_object_get_ex(core_daemon, "proto", &proto_jobj);
	int service_proto = get_proto_type(proto_jobj ? json_object_get_string(proto_jobj) : "");

	json_object *timeout_jobj = NULL;
	json_object_object_get_ex(core_daemon, "timeout", &timeout_jobj);
	int service_timeout = timeout_jobj ? json_object_get_int(timeout_jobj) : SERVICE_CALL_TIMEOUT;

	/* count the total number of service objects across
	 * core.json + all enabled peer files so we can allocate once. */
	size_t total_capacity = 0;

	/* Count objects in core.json's services array */
	json_object *core_services = NULL;
	json_object_object_get_ex(core_daemon, "services", &core_services);
	if (core_services && json_object_get_type(core_services) == json_type_array)
		total_capacity += json_object_array_length(core_services);

	/* Count objects from every other enabled file */
	json_object **peer_roots = calloc(fl->count, sizeof(json_object *)); /* index 0 unused */
	if (!peer_roots) {
		BBFDM_ERR("Failed to allocate peer_roots");
		json_object_put(core_root);
		return;
	}

	for (size_t i = 1; i < fl->count; i++) {
		json_object *peer_root = json_object_from_file(fl->paths[i]);
		if (!peer_root) {
			BBFDM_WARNING("Failed to read JSON: %s – skipping", fl->paths[i]);
			continue;
		}

		json_object *peer_daemon = NULL;
		json_object_object_get_ex(peer_root, "daemon", &peer_daemon);
		if (!peer_daemon) {
			json_object_put(peer_root);
			continue;
		}

		json_object *enable_jobj = NULL;
		json_object_object_get_ex(peer_daemon, "enable", &enable_jobj);
		bool enabled = enable_jobj ? json_object_get_boolean(enable_jobj) : false;
		if (!enabled) {
			BBFDM_INFO("Service '%s' is disabled – skipping", fl->filenames[i]);
			json_object_put(peer_root);
			continue;
		}

		json_object *peer_services = NULL;
		json_object_object_get_ex(peer_daemon, "services", &peer_services);
		if (peer_services && json_object_get_type(peer_services) == json_type_array)
			total_capacity += json_object_array_length(peer_services);

		peer_roots[i] = peer_root; /* retain for the fill pass below */
	}

	if (total_capacity == 0) {
		BBFDM_WARNING("No service objects found across non-unified files – skipping");
		for (size_t i = 1; i < fl->count; i++) {
			if (peer_roots[i])
				json_object_put(peer_roots[i]);
		}

		BBFDM_FREE(peer_roots);
		json_object_put(core_root);
		return;
	}

	service_object_t *objects = calloc(total_capacity, sizeof(service_object_t));
	if (!objects) {
		BBFDM_ERR("Failed to allocate service objects");
		for (size_t i = 1; i < fl->count; i++) {
			if (peer_roots[i])
				json_object_put(peer_roots[i]);
		}

		BBFDM_FREE(peer_roots);
		json_object_put(core_root);
		return;
	}

	/* fill the objects array */
	size_t num_objs = 0;

	/* core.json objects */
	if (core_services && json_object_get_type(core_services) == json_type_array)
		fill_from_services(core_services, "core.json", objects, &num_objs, total_capacity);

	/* peer files */
	for (size_t i = 1; i < fl->count; i++) {
		if (!peer_roots[i])
			continue;

		json_object *peer_daemon = NULL;
		json_object_object_get_ex(peer_roots[i], "daemon", &peer_daemon);

		json_object *peer_services = NULL;
		json_object_object_get_ex(peer_daemon, "services", &peer_services);

		if (peer_services && json_object_get_type(peer_services) == json_type_array)
			fill_from_services(peer_services, fl->filenames[i], objects, &num_objs, total_capacity);

		json_object_put(peer_roots[i]);
	}

	BBFDM_FREE(peer_roots);
	json_object_put(core_root);

	/* Register the single merged service entry */
	BBFDM_INFO("Registering non-unified service [%s :: %zu objects]", service_name, num_objs);
	add_service_to_list(service_name, service_schema, service_proto, service_timeout, objects, num_objs, false);
}

static int load_unified_service(struct ubus_context *ubus_ctx, const char *filename, const char *file_path)
{
	size_t num_objs = 0;

	if (!filename || !file_path) {
		BBFDM_ERR("Invalid filename or file path");
		return -1;
	}

	json_object *json_root = json_object_from_file(file_path);
	if (!json_root) {
		BBFDM_ERR("Failed to read JSON file: %s", file_path);
		return -1;
	}

	json_object *daemon_config = NULL;
	json_object_object_get_ex(json_root, "daemon", &daemon_config);
	if (!daemon_config) {
		BBFDM_ERR("Failed to find daemon object in: %s", file_path);
		json_object_put(json_root);
		return -1;
	}

	json_object *enable_jobj = NULL;
	json_object_object_get_ex(daemon_config, "enable", &enable_jobj);
	bool enable = enable_jobj ? json_object_get_boolean(enable_jobj) : false;
	if (!enable) {
		BBFDM_INFO("Unified service '%s' is disabled – skipping", filename);
		json_object_put(json_root);
		return -1;
	}

	char service_name[MAX_PATH_LENGTH] = {0};
	snprintf(service_name, sizeof(service_name), "%s.%.*s", BBFDM_UBUS_OBJECT, (int)(strlen(filename) - 5), filename);

	struct blob_buf *service_schema = NULL;
	fill_service_schema(ubus_ctx, 2000, service_name, &service_schema);

	json_object *proto_jobj = NULL;
	json_object_object_get_ex(daemon_config, "proto", &proto_jobj);
	int service_proto = get_proto_type(proto_jobj ? json_object_get_string(proto_jobj) : "");

	json_object *timeout_jobj = NULL;
	json_object_object_get_ex(daemon_config, "timeout", &timeout_jobj);
	int service_timeout = timeout_jobj ? json_object_get_int(timeout_jobj) : SERVICE_CALL_TIMEOUT;

	json_object *services_array = NULL;
	if (!json_object_object_get_ex(daemon_config, "services", &services_array) ||
	    json_object_get_type(services_array) != json_type_array) {
		BBFDM_WARNING("No valid 'services' array in: %s", filename);
		json_object_put(json_root);
		return -1;
	}

	size_t service_count = json_object_array_length(services_array);
	if (service_count == 0) {
		BBFDM_WARNING("Empty 'services' array in unified service '%s'", service_name);
		json_object_put(json_root);
		return -1;
	}

	service_object_t *objects = calloc(service_count, sizeof(service_object_t));
	if (!objects) {
		BBFDM_ERR("Failed to allocate memory");
		json_object_put(json_root);
		return -1;
	}

	for (size_t i = 0; i < service_count; i++) {
		json_object *service_obj = json_object_array_get_idx(services_array, i);
		json_object *parent_dm = NULL, *object = NULL, *proto = NULL;

		json_object_object_get_ex(service_obj, "parent_dm", &parent_dm);
		json_object_object_get_ex(service_obj, "object", &object);
		json_object_object_get_ex(service_obj, "proto", &proto);

		snprintf(objects[num_objs].parent_path, sizeof(objects[num_objs].parent_path), "%s", parent_dm ? json_object_get_string(parent_dm) : "");
		snprintf(objects[num_objs].object_name, sizeof(objects[num_objs].object_name), "%s", object ? json_object_get_string(object) : "");

		if (!strlen(objects[num_objs].parent_path) || !strlen(objects[num_objs].object_name)) {
			BBFDM_WARNING("Skip empty parent_dm/object in '%s'", service_name);
			continue;
		}

		objects[num_objs].protocol = get_proto_type(proto ? json_object_get_string(proto) : "");
		num_objs++;
	}

	BBFDM_INFO("Registering unified service [%s :: %zu objects]", service_name, num_objs);
	add_service_to_list(service_name, service_schema, service_proto, service_timeout, objects, num_objs, true);

	json_object_put(json_root);
	return 0;
}

int register_suppress_services(struct ubus_context *ubus_ctx)
{
	struct dirent **namelist = NULL;
	file_list_t non_unified_list = {0};
	file_list_t unified_list = {0};

	int num_files = scandir(BBFDM_MICROSERVICE_INPUT_PATH, &namelist, filter, compare);
	if (num_files < 0) {
		BBFDM_ERR("scandir failed on: %s", BBFDM_MICROSERVICE_INPUT_PATH);
		return -1;
	}

	if (file_list_init(&non_unified_list, (size_t)num_files) < 0 || file_list_init(&unified_list, (size_t)num_files) < 0) {
		BBFDM_ERR("Failed to allocate categorisation lists");
		for (int i = 0; i < num_files; i++)
			BBFDM_FREE(namelist[i]);

		BBFDM_FREE(namelist);

		file_list_free(&non_unified_list);
		file_list_free(&unified_list);
		return -1;
	}

	/* categorise every valid JSON file */
	for (int i = 0; i < num_files; i++) {
		char file_path[512] = {0};
		snprintf(file_path, sizeof(file_path), "%s/%s", BBFDM_MICROSERVICE_INPUT_PATH, namelist[i]->d_name);

		if (!bbfdm_file_exists(file_path) || !bbfdm_is_regular_file(file_path)) {
			BBFDM_FREE(namelist[i]);
			continue;
		}

		bool is_unified = peek_is_unified(file_path);
		file_list_t *target = is_unified ? &unified_list : &non_unified_list;

		if (file_list_push(target, file_path, namelist[i]->d_name) < 0)
			BBFDM_ERR("Failed to push '%s' into categorisation list", namelist[i]->d_name);

		BBFDM_FREE(namelist[i]);
	}
	BBFDM_FREE(namelist);

	/* handle all non-unified files as a single merged service */
	load_non_unified_services(ubus_ctx, &non_unified_list);

	/* handle each unified file as its own independent service */
	for (size_t i = 0; i < unified_list.count; i++) {
		if (load_unified_service(ubus_ctx, unified_list.filenames[i], unified_list.paths[i]))
			BBFDM_ERR("Failed to load unified service: %s", unified_list.filenames[i]);
	}

	file_list_free(&non_unified_list);
	file_list_free(&unified_list);
	return 0;
}

int register_services(struct ubus_context *ubus_ctx)
{
	struct dirent **namelist;

	int num_files = scandir(BBFDM_MICROSERVICE_INPUT_PATH, &namelist, filter, compare);

	for (int i = 0; i < num_files; i++) {
		char file_path[512] = {0};

		snprintf(file_path, sizeof(file_path), "%s/%s", BBFDM_MICROSERVICE_INPUT_PATH, namelist[i]->d_name);

		if (!bbfdm_file_exists(file_path) || !bbfdm_is_regular_file(file_path)) {
			BBFDM_FREE(namelist[i]);
			continue;
		}

		if (load_service_from_file(ubus_ctx, namelist[i]->d_name, file_path)) {
			BBFDM_ERR("Failed to load service: %s", namelist[i]->d_name);
		}

		BBFDM_FREE(namelist[i]);
	}

	BBFDM_FREE(namelist);
	return 0;
}

void unregister_services(void)
{
    service_entry_t *service = NULL, *tmp = NULL;

    list_for_each_entry_safe(service, tmp, &registered_services, list) {
        list_del(&service->list);

        if (service->dm_schema) {
            blob_buf_free(service->dm_schema);
            BBFDM_FREE(service->dm_schema);
        }

        BBFDM_FREE(service->name);
        BBFDM_FREE(service->objects);
        BBFDM_FREE(service);
    }
}

void list_registered_services(struct blob_buf *bb)
{
	service_entry_t *service = NULL;

	if (!bb)
		return;

	void *array = blobmsg_open_array(bb, "registered_services");

	list_for_each_entry(service, &registered_services, list) {
		void *table = blobmsg_open_table(bb, NULL);

		blobmsg_add_string(bb, "name", service->name ? service->name : "");
		blobmsg_add_string(bb, "proto",
			service->protocol == BBFDMD_USP ? "usp" :
			service->protocol == BBFDMD_CWMP ? "cwmp" : "both");

		blobmsg_add_u8(bb, "unified_daemon", service->is_unified);
		blobmsg_add_u8(bb, "blacklisted", service->is_blacklisted);
		blobmsg_add_u32(bb, "timeout", service->timeout);

		void *objects_array = blobmsg_open_array(bb, "objects");
		for (size_t i = 0; i < service->object_count; i++) {
			void *obj_table = blobmsg_open_table(bb, NULL);
			blobmsg_add_string(bb, "parent_dm", service->objects[i].parent_path);
			blobmsg_add_string(bb, "object", service->objects[i].object_name);

			if (service->protocol == BBFDMD_USP) {
				blobmsg_add_string(bb, "proto", "usp");
			} else if (service->protocol == BBFDMD_CWMP) {
				blobmsg_add_string(bb, "proto", "cwmp");
			} else {
				blobmsg_add_string(bb, "proto",
					service->objects[i].protocol == BBFDMD_USP ? "usp" :
					service->objects[i].protocol == BBFDMD_CWMP ? "cwmp" : "both");
			}

			blobmsg_close_table(bb, obj_table);
		}
		blobmsg_close_array(bb, objects_array);

		blobmsg_close_table(bb, table);
	}

	blobmsg_close_array(bb, array);
}

bool service_path_match(const char *requested_path, unsigned int requested_proto, service_entry_t *service)
{
	if (!proto_match(requested_proto, service->protocol))
		return false;

	if (strlen(requested_path) == 0 || strcmp(requested_path, BBFDM_ROOT_OBJECT) == 0)
		return true;

	if (strncmp(BBFDM_ROOT_OBJECT, requested_path, strlen(BBFDM_ROOT_OBJECT)) != 0)
		return false;

	for (size_t idx = 0; idx < service->object_count; idx++) {
		char current_obj[MAX_PATH_LENGTH] = {0};

		if (!proto_match(requested_proto, service->objects[idx].protocol))
			continue;

		snprintf(current_obj, sizeof(current_obj), "%s%s", service->objects[idx].parent_path, service->objects[idx].object_name);

		if (strncmp(current_obj, requested_path, strlen(current_obj)) == 0)
			return true;

		if (strncmp(requested_path, current_obj, strlen(requested_path)) == 0)
			return true;
	}

	return false;
}
