/*
 * bbf_config.c: bbf.config daemon
 *
 * Copyright (C) 2024 IOPSYS Software Solutions AB. All rights reserved.
 *
 * Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 * See LICENSE file for license related information.
 */

#include <stdio.h>
#include <unistd.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <json-c/json.h>
#include <dirent.h>

#include "utils.h"

#define TIME_TO_WAIT_FOR_RELOAD 5
#define MAX_PACKAGE_NUM 256
#define MAX_SERVICE_NUM 16
#define MAX_INSTANCE_NUM 8
#define NAME_LENGTH 64
#define DEFAULT_LOG_LEVEL LOG_INFO

#define BBF_CONFIG_DAEMON_NAME "bbf_configd"
#define CRITICAL_DEF_JSON "/etc/bbfdm/critical_services.json"
#define BBFDM_MICROSERVICE_INPUT_PATH "/etc/bbfdm/services"

static struct list_head g_external_changed_uci;

// Structure to represent an instance of a service
struct instance {
	char name[NAME_LENGTH];
	uint32_t pid;
	bool is_running;
};

// Structure to represent a service
struct service {
	char name[NAME_LENGTH];
	bool has_instances;
	struct instance instances[MAX_INSTANCE_NUM];
};

// Structure to represent a configuration package
struct config_package {
	char name[NAME_LENGTH];
	struct service services[MAX_SERVICE_NUM];
};

struct bbf_config_async_req {
	int idx;
	struct ubus_context *ctx;
	struct ubus_request_data req;
	struct uloop_timeout timeout;
	struct blob_attr *services;
	struct ubus_event_handler sync_ev;
	struct config_package package[MAX_PACKAGE_NUM];
	struct list_head changed_uci_list;
};

static struct blob_buf g_critical_bb;
static struct list_head g_apply_handlers;
static struct list_head g_revert_handlers;
static struct list_head g_pre_apply_handlers;

#ifdef BBF_CONFIG_DEBUG
static void log_instance(struct instance *inst)
{
	ULOG_ERR("    |- Instance name: '%s', PID: %d, Status: %s",
			 inst->name,
			 inst->pid,
			 inst->is_running ? "running" : "stopped");
}

static void log_service(struct service *svc)
{
	ULOG_ERR("  - Service name: '%s'", svc->name);
	if (svc->has_instances) {
		bool has_any_instance = false;
		for (int i_idx = 0; i_idx < MAX_INSTANCE_NUM; i_idx++) {
			if (svc->instances[i_idx].name[0] == '\0')
				break;

			log_instance(&svc->instances[i_idx]);
			has_any_instance = true;
		}
		if (!has_any_instance) {
			ULOG_ERR("    |- No active instances");
		}
	} else {
		ULOG_ERR("    |- No instances available");
	}
}

static void show_package_tree(struct config_package *packages)
{
	for (int p_idx = 0; p_idx < MAX_PACKAGE_NUM; p_idx++) {

		if (packages[p_idx].name[0] == '\0')
			break;

		ULOG_ERR("Package name: '%s'", packages[p_idx].name);
		bool has_any_service = false;
		for (int s_idx = 0; s_idx < MAX_SERVICE_NUM; s_idx++) {
			if (packages[p_idx].services[s_idx].name[0] == '\0')
				break;

			log_service(&packages[p_idx].services[s_idx]);
			has_any_service = true;
		}

		if (!has_any_service) {
			ULOG_ERR("  |- No services defined");
		}
	}
}
#endif

static bool g_internal_commit = false;

enum {
	SERVICES_NAME,
	SERVICES_PROTO,
	SERVICES_RELOAD,
	__MAX
};

static const struct blobmsg_policy bbf_config_policy[] = {
	[SERVICES_NAME] = { .name = "services", .type = BLOBMSG_TYPE_ARRAY },
	[SERVICES_PROTO] = { .name = "proto", .type = BLOBMSG_TYPE_STRING },
	[SERVICES_RELOAD] = { .name = "reload", .type = BLOBMSG_TYPE_BOOL },
};

static int find_config_idx(struct config_package *package, const char *config_name)
{
	if (!config_name)
		return -1;

	for (int i = 0; i < MAX_PACKAGE_NUM; i++) {

		if (strlen(package[i].name) == 0)
			return -1;

		if (strcmp(package[i].name, config_name) == 0)
			return i;
	}

	return -1;
}

static int find_service_idx(struct service *services)
{
	if (!services)
		return -1;

	for (int i = 0; i < MAX_SERVICE_NUM; i++) {

		if (strlen(services[i].name) == 0)
			return i;
	}

	return -1;
}

static int find_instance_idx(struct instance *instances, const char *instance_name)
{
	if (!instances)
		return -1;

	for (int i = 0; i < MAX_INSTANCE_NUM; i++) {

		if (instance_name && strcmp(instances[i].name, instance_name) == 0)
			return i;

		if (strlen(instances[i].name) == 0)
			return i;
	}

	return -1;
}

static int handle_instances_service(const char *service_name, struct blob_attr *instances, struct config_package *package, unsigned int pkg_idx)
{
	int srv_idx = find_service_idx(package[pkg_idx].services);

	if (srv_idx < 0) { // Returns if the number of services more than MAX_SERVICE_NUM
		ULOG_ERR("Failed to handle instance service: service count exceeds MAX_SERVICE_NUM");
		return -1;
	}

	strncpyt(package[pkg_idx].services[srv_idx].name, service_name, NAME_LENGTH);
	package[pkg_idx].services[srv_idx].has_instances = (instances) ? true : false;

	if (!instances)
		return -1;

	struct blob_attr *cur;
	int rem;

	int inst_idx = find_instance_idx(package[pkg_idx].services[srv_idx].instances, NULL);

	if (inst_idx < 0) // Returns if the number of instances more than MAX_INSTANCE_NUM
		return -1;

	blobmsg_for_each_attr(cur, instances, rem) {

		struct blob_attr *tb[2] = {0};
		const struct blobmsg_policy p[2] = {
				{ "running", BLOBMSG_TYPE_BOOL },
				{ "pid", BLOBMSG_TYPE_INT32 }
		};

		blobmsg_parse(p, 2, tb, blobmsg_data(cur), blobmsg_len(cur));

		strncpyt(package[pkg_idx].services[srv_idx].instances[inst_idx].name, blobmsg_name(cur), NAME_LENGTH);
		package[pkg_idx].services[srv_idx].instances[inst_idx].is_running = (tb[0]) ? blobmsg_get_bool(tb[0]) : false;
		package[pkg_idx].services[srv_idx].instances[inst_idx].pid = (tb[1]) ? blobmsg_get_u32(tb[1]) : 0;
		inst_idx++;
	}


	return 0;
}

static int handle_triggers_service(const char *service_name, struct blob_attr *triggers, struct blob_attr *instances, struct config_package *package, unsigned int *index)
{
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, triggers, rem) {
		struct blob_attr *_cur, *type = NULL, *script = NULL, *config = NULL, *name = NULL;
		size_t _rem;
		int i = 0;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_ARRAY)
			continue;

		blobmsg_for_each_attr(_cur, cur, _rem) {
			switch (i++) {
			case 0:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_STRING)
					type = _cur;
				break;

			case 1:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_ARRAY)
					script = _cur;
				break;
			}
		}

		if (!type || !script || strcmp(blobmsg_get_string(type), "config.change") != 0)
			continue;

		type = NULL;
		i = 0;

		blobmsg_for_each_attr(_cur, script, _rem) {
			switch (i++) {
			case 0:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_STRING)
					type = _cur;
				break;

			case 1:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_ARRAY)
					config = _cur;
				break;
			}
		}

		if (!type || !config || strcmp(blobmsg_get_string(type), "if") != 0)
			continue;

		type = NULL;
		script = NULL;
		i = 0;

		blobmsg_for_each_attr(_cur, config, _rem) {
			switch (i++) {
			case 0:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_STRING)
					type = _cur;
				break;

			case 1:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_STRING)
					script = _cur;
				break;

			case 2:
				if (blobmsg_type(_cur) == BLOBMSG_TYPE_STRING)
					name = _cur;
				break;
			}
		}

		if (!type || !script || !name ||
				strcmp(blobmsg_get_string(type), "eq") != 0 ||
				strcmp(blobmsg_get_string(script), "package") != 0)
			continue;

		char *config_name = blobmsg_get_string(name);

		int config_idx = find_config_idx(package, config_name);

		unsigned int pkg_idx = (config_idx < 0) ? (*index)++ : config_idx;

		strncpyt(package[pkg_idx].name, blobmsg_get_string(name), NAME_LENGTH);

		handle_instances_service(service_name, instances, package, pkg_idx);
	}

	return 0;
}

static void _get_service_list_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *cur;
	struct blob_attr *tb[2] = {0};
	const struct blobmsg_policy p[2] = {
			{ "triggers", BLOBMSG_TYPE_ARRAY },
			{ "instances", BLOBMSG_TYPE_TABLE }
	};
	size_t rem;
	unsigned int idx = 0;

	if (!msg || !req) {
		ULOG_ERR("Cannot proceed: 'msg' or 'req' is NULL.");
		return;
	}

	struct config_package *package = (struct config_package *)req->priv;

	blobmsg_for_each_attr(cur, msg, rem) {

		blobmsg_parse(p, 2, tb, blobmsg_data(cur), blobmsg_len(cur));

		if (!tb[0])
			continue;

		handle_triggers_service(blobmsg_name(cur), tb[0], tb[1], package, &idx);
	}
}

static void _get_specific_service_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct blob_attr *cur;
	struct blob_attr *tb[1] = {0};
	const struct blobmsg_policy p[1] = {
			{ "instances", BLOBMSG_TYPE_TABLE }
	};
	size_t rem;

	if (!msg || !req) {
		ULOG_ERR("Cannot proceed: 'msg' or 'req' is NULL.");
		return;
	}

	struct config_package *package = (struct config_package *)req->priv;

	blobmsg_for_each_attr(cur, msg, rem) {

		blobmsg_parse(p, 1, tb, blobmsg_data(cur), blobmsg_len(cur));

		handle_instances_service(blobmsg_name(cur), tb[0], package, 0);
	}
}

static void fill_service_info(struct ubus_context *ctx, struct config_package *package, const char *name, bool verbose, ubus_data_handler_t callback)
{
	struct blob_buf ubus_bb = {0};

	memset(&ubus_bb, 0 , sizeof(struct blob_buf));
	blob_buf_init(&ubus_bb, 0);

	if (name) blobmsg_add_string(&ubus_bb, "name", name);

	blobmsg_add_u8(&ubus_bb, "verbose", verbose);

	bbf_config_call(ctx, "service", "list", &ubus_bb, callback, (void *)package);

	blob_buf_free(&ubus_bb);
}

static bool validate_required_services(struct ubus_context *ctx, struct config_package *package, struct blob_attr *services)
{
	struct blob_attr *service = NULL;
	size_t rem = 0;

	if (!services || !package)
		return true;

	// Iterate through each service attribute
	blobmsg_for_each_attr(service, services, rem) {
		char *config_name = blobmsg_get_string(service);
		char *p = strrchr(config_name, '/');
		if (p) {
			p = p + 1;
		}

		// Find the index of the configuration package
		int idx = find_config_idx(package, p ? p : config_name);
		if (idx < 0)
			continue;

		for (int j = 0; j < MAX_SERVICE_NUM && strlen(package[idx].services[j].name); j++) {

			if (strcmp(package[idx].services[j].name, BBF_CONFIG_DAEMON_NAME) == 0) {
				// Skip 'bbf_configd' service itself, as it does not need processing here
				continue; // Move to the next service
			}

			// Get configuration information for each service name
			struct config_package new_package[1] = {0};

			memset(new_package, 0, sizeof(struct config_package));

			fill_service_info(ctx, new_package, package[idx].services[j].name, false, _get_specific_service_cb);

			if (package[idx].services[j].has_instances != new_package[0].services[0].has_instances) {
				// If the number of instances has changed, the service is correctly updated
				continue; // Move to the next service
			}

			if (package[idx].services[j].has_instances == 0) {
				// No instances to check, unsure if service is correctly updated
				goto wait;
			}

			for (int t = 0; t < MAX_SERVICE_NUM && strlen(package[idx].services[j].instances[t].name); t++) {

				// Find the index of the instance in the new package
				int inst_idx = find_instance_idx(new_package[0].services[0].instances,
						package[idx].services[j].instances[t].name);

				if (inst_idx < 0) {
					// Instance doesn't exist after reload, indicating a disabled instance
					continue; // Move to the next service
				}

				if (package[idx].services[j].instances[t].is_running != new_package[0].services[0].instances[inst_idx].is_running) {
					// Instance status changed after reload, service correctly updated
					continue; // Move to the next service
				}

				if (package[idx].services[j].instances[t].pid != new_package[0].services[0].instances[inst_idx].pid) {
					// Instance PID changed after reload, service correctly updated
					continue; // Move to the next service
				}

				// Wait for a sufficient time to ensure services are reloaded
				goto wait;
			}
		}
	}

	return true;

wait:
	return false;
}

static int send_bbf_apply_event(int idx, struct list_head *changed_uci_list)
{
	char protocol[16] = {0};

	if (changed_uci_list == NULL || list_empty(changed_uci_list))
		return -1;

	struct ubus_context *ctx;
	struct blob_buf bb = {0};
	struct modi_uci_node *node = NULL, *tmp = NULL;

	memset(&bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&bb, 0);

	snprintf(protocol, sizeof(protocol), "%s", (idx == -1) ? "external" : get_proto_name_by_idx(idx));

	blobmsg_add_string(&bb, "proto", protocol);
	void *array = blobmsg_open_array(&bb, "uci_changed");
	list_for_each_entry_safe(node, tmp, changed_uci_list, list) {
		if (node->uci == NULL) {
			list_del(&node->list);
			FREE(node);
			continue;
		}

		blobmsg_add_string(&bb, NULL, node->uci);
		list_del(&node->list);
		FREE(node->uci);
		FREE(node);
	}
	blobmsg_close_array(&bb, array);

	ctx = ubus_connect(NULL);
	if (ctx == NULL) {
		ULOG_ERR("Can't create UBUS context for 'bbfdm.apply' event");
		blob_buf_free(&bb);
		return -1;
	}

	ULOG_INFO("Sending bbfdm.apply event");

	ubus_send_event(ctx, "bbfdm.apply", bb.head);
	blob_buf_free(&bb);
	ubus_free(ctx);

	return 0;
}

static void send_reply(struct ubus_context *ctx, struct ubus_request_data *req, const char *message, const char *description)
{
	struct blob_buf bb = {0};

	memset(&bb, 0, sizeof(struct blob_buf));

	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, message, description);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
}

static void sync_event_handler(struct ubus_context *ctx, struct ubus_event_handler *ev,
			const char *type, struct blob_attr *msg)
{
	if (msg == NULL || ev == NULL)
		return;

	struct bbf_config_async_req *async_req = container_of(ev, struct bbf_config_async_req, sync_ev);
	if (async_req == NULL)
		return;

	// validate if the event received from core service
	const struct blobmsg_policy p[1] = {
		{ "service", BLOBMSG_TYPE_STRING }
	};

	struct blob_attr *tb[1] = {0};
	if (blobmsg_parse(p, 1, tb, blob_data(msg), blob_len(msg)) != 0)
		return;

	if (!tb[0] || strcmp("core", blobmsg_get_string(tb[0])) != 0) {
		return;
	}

	uloop_timeout_cancel(&(async_req->timeout));

	// Send the response
	send_reply(async_req->ctx, &async_req->req, "status", "ok");

	// Complete the deferred request and send the response
	ubus_complete_deferred_request(async_req->ctx, &async_req->req, 0);

	ubus_unregister_event_handler(async_req->ctx, &(async_req->sync_ev));

	// Free the allocated memory
	FREE(async_req->services);
	FREE(async_req);

	ULOG_INFO("Commit handler exit");
}

static void sync_listen_timeout(struct uloop_timeout *timeout)
{
	struct bbf_config_async_req *async_req = container_of(timeout, struct bbf_config_async_req, timeout);
	if (async_req == NULL)
		return;

	ULOG_INFO("Timeout occurred for sync_complete event from core");

	// Send the response
	send_reply(async_req->ctx, &async_req->req, "status", "ok");

	// Complete the deferred request and send the response
	ubus_complete_deferred_request(async_req->ctx, &async_req->req, 0);

	ubus_unregister_event_handler(async_req->ctx, &(async_req->sync_ev));

	// Free the allocated memory
	FREE(async_req->services);
	FREE(async_req);

	ULOG_INFO("Commit handler exit");
}

static void wait_for_sync_complete_event(struct bbf_config_async_req *async_req)
{
	if (async_req == NULL)
		return;

	if (ubus_register_event_handler(async_req->ctx, &(async_req->sync_ev), "dmservice.sync_complete")) {
		ULOG_ERR("Failed to register dmservice.sync_complete event handler");
		// Send 'bbf.apply' event
		send_bbf_apply_event(async_req->idx, &async_req->changed_uci_list);
		goto exit;
	}

	uloop_timeout_set(&(async_req->timeout), 5000);

	// Send 'bbf.apply' event
	if (0 != send_bbf_apply_event(async_req->idx, &async_req->changed_uci_list)) {
		uloop_timeout_cancel(&(async_req->timeout));
		ubus_unregister_event_handler(async_req->ctx, &(async_req->sync_ev));
		goto exit;
	}

	return;

exit:
	// Send the response
	send_reply(async_req->ctx, &async_req->req, "status", "ok");

	// Complete the deferred request and send the response
	ubus_complete_deferred_request(async_req->ctx, &async_req->req, 0);

	// Free the allocated memory
	FREE(async_req->services);
	FREE(async_req);

	ULOG_INFO("Commit handler exit");
}

static void complete_deferred_request(struct bbf_config_async_req *async_req)
{
	if (!async_req)
		return;

	// If any uci is changed externally then add it in bbf.apply event
	struct modi_uci_node *node = NULL, *tmp = NULL;
	list_for_each_entry_safe(node, tmp, &g_external_changed_uci, list) {
		if (node->uci == NULL) {
			list_del(&node->list);
			FREE(node);
			continue;
		}

		add_changed_uci_list(&async_req->changed_uci_list, node->uci);
		list_del(&node->list);
		FREE(node->uci);
		FREE(node);
	}

	// Lets wait for sync_complete from core
	async_req->timeout.cb = sync_listen_timeout;
	async_req->sync_ev.cb = sync_event_handler;

	// Set internal commit to false
	g_internal_commit = false;

	wait_for_sync_complete_event(async_req);
}

static void end_request_callback(struct uloop_timeout *t)
{
	struct bbf_config_async_req *async_req = container_of(t, struct bbf_config_async_req, timeout);

	// Complete the deferred request and send the reply
	complete_deferred_request(async_req);
}

static void complete_request_callback(struct uloop_timeout *t)
{
	struct bbf_config_async_req *async_req = container_of(t, struct bbf_config_async_req, timeout);

	// Check if the required services are really reloaded
	bool reload_done = validate_required_services(async_req->ctx, async_req->package, async_req->services);

	if (reload_done) {
		// Complete the deferred request and send the reply
		complete_deferred_request(async_req);
	} else {
		async_req->timeout.cb = end_request_callback;
		uloop_timeout_set(&async_req->timeout, TIME_TO_WAIT_FOR_RELOAD * 1000);
	}
}

struct blob_attr *get_blob_attr_with_idx(int idx, struct blob_attr *msg)
{
	struct blob_attr *params = NULL;
	struct blob_attr *cur;
	int rem;
	const char *proto = get_proto_name_by_idx(idx);

	blobmsg_for_each_attr(cur, msg, rem) {
		const char *name = blobmsg_name(cur);

		if ((strcmp(proto, name) == 0) && (blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)) {
			params = cur;
			break;
		}
	}
	return params;
}

static bool check_if_critical_service(int proto_idx, const char *sname)
{
	bool is_critical = false;
	struct blob_attr *service = NULL, *services_ba;
	size_t rem = 0;

	services_ba = get_blob_attr_with_idx(proto_idx, g_critical_bb.head);
	if (services_ba == NULL) {
		ULOG_DEBUG("Critical service not defined for %s proto", get_proto_name_by_idx(proto_idx));
		return is_critical;
	}

	blobmsg_for_each_attr(service, services_ba, rem) {
		char *config_name = blobmsg_get_string(service);

		if (strcmp(sname, config_name) == 0) {
			is_critical = true;
			break;
		}
	}
	ULOG_DEBUG("Service %s, found %d, in %s critical list", sname, is_critical, get_proto_name_by_idx(proto_idx));
	return is_critical;
}

static bool get_monitor_status(int proto_idx, struct blob_attr *services_ba)
{
	bool monitor = false, is_critical;
	struct blob_attr *service = NULL;
	size_t rem = 0;

	if (services_ba == NULL) {
		ULOG_DEBUG("Empty service list");
		return monitor;
	}

	blobmsg_for_each_attr(service, services_ba, rem) {
		char *config_name = blobmsg_get_string(service);

		is_critical = check_if_critical_service(proto_idx, config_name);
		if (is_critical == true) {
			monitor = true;
			break;
		}
	}
	return monitor;
}

static bool service_list_contains_config(struct blob_attr *services, const char *config)
{
	struct blob_attr *service = NULL;
	size_t rem = 0;

	if (services == NULL || config == NULL || config[0] == '\0') {
		return false;
	}

	blobmsg_for_each_attr(service, services, rem) {
		const char *svc;
		const char *svc_base;

		if (blobmsg_type(service) != BLOBMSG_TYPE_STRING) {
			continue;
		}

		svc = blobmsg_get_string(service);
		if (svc == NULL || svc[0] == '\0') {
			continue;
		}

		// Ignore dmmap entries (we only want to match against UCI elements)
		if (strncmp(svc, DMMAP_CONFDIR, strlen(DMMAP_CONFDIR)) == 0) {
			continue;
		}

		svc_base = strrchr(svc, '/');
		svc_base = (svc_base != NULL) ? (svc_base + 1) : svc;

		if (strcmp(svc_base, config) == 0) {
			return true;
		}
	}

	return false;
}

static void execute_pre_apply_handlers(struct blob_attr *services)
{
	struct list_head pre_action_list;
	struct applier_node *app_node = NULL;
	struct action_node *act_node = NULL, *tmp = NULL;
	bool services_specified;
	bool uci_element_specified = false;

	services_specified = (services != NULL) && (blobmsg_len(services) > 0);
	if (services_specified) {
		struct blob_attr *service = NULL;
		size_t rem = 0;

		blobmsg_for_each_attr(service, services, rem) {
			const char *svc;

			if (blobmsg_type(service) != BLOBMSG_TYPE_STRING) {
				continue;
			}

			svc = blobmsg_get_string(service);
			if (svc == NULL || svc[0] == '\0') {
				continue;
			}

			// Ignore dmmap entries (we only want to match against UCI elements)
			if (strncmp(svc, DMMAP_CONFDIR, strlen(DMMAP_CONFDIR)) == 0) {
				continue;
			}

			uci_element_specified = true;
			break;
		}
	}
	INIT_LIST_HEAD(&pre_action_list);

	list_for_each_entry(app_node, &g_pre_apply_handlers, list) {
		const char *config;
		bool trigger;

		if (app_node->action == NULL) {
			continue;
		}

		// If services are specified but none are UCI elements (e.g. only dmmap entries),
		// still call the pre apply handler once (per action)
		if (services_specified && (uci_element_specified == false)) {
			bool node_exist = false;
			list_for_each_entry(act_node, &pre_action_list, list) {
				if (strcmp(act_node->action, app_node->action) == 0) {
					node_exist = true;
					break;
				}
			}

			if (node_exist == false) {
				act_node = (struct action_node *)calloc(1, sizeof(struct action_node));
				if (act_node == NULL) {
					ULOG_INFO("Failed to allocate memory for pre apply action list");
					continue;
				}

				snprintf(act_node->action, sizeof(act_node->action), "%s", app_node->action);
				INIT_LIST_HEAD(&act_node->list);
				list_add_tail(&act_node->list, &pre_action_list);
			}

			continue;
		}

		if (app_node->file_path == NULL) {
			continue;
		}

		config = strrchr(app_node->file_path, '/');
		config = (config != NULL) ? (config + 1) : app_node->file_path;
		if (config == NULL || config[0] == '\0') {
			continue;
		}

		// Trigger if no service is specified, or if any configured UCI file is included in the services list
		trigger = (!services_specified) ? true : service_list_contains_config(services, config);
		if (trigger == false) {
			continue;
		}

		// Group by action, and pass all matched configs as args (handler must be called only once)
		bool node_exist = false;
		list_for_each_entry(act_node, &pre_action_list, list) {
			if (strcmp(act_node->action, app_node->action) == 0) {
				node_exist = true;
				break;
			}
		}

		if (node_exist == false) {
			act_node = (struct action_node *)calloc(1, sizeof(struct action_node));
			if (act_node == NULL) {
				ULOG_INFO("Failed to allocate memory for pre apply action list");
				continue;
			}

			snprintf(act_node->action, sizeof(act_node->action), "%s", app_node->action);
			INIT_LIST_HEAD(&act_node->list);
			list_add_tail(&act_node->list, &pre_action_list);
		}

		// Add arg if not already present
		bool arg_exist = false;
		for (int i = 0; i < act_node->idx; i++) {
			if (strcmp(act_node->arg[i], config) == 0) {
				arg_exist = true;
				break;
			}
		}

		if (arg_exist == false && act_node->idx < ARG_COUNT) {
			snprintf(act_node->arg[act_node->idx], ARG_LEN, "%s", config);
			act_node->idx++;
		}
	}

	// Execute all pre apply handlers once (per action)
	list_for_each_entry_safe(act_node, tmp, &pre_action_list, list) {
		char cmd[4096] = {0};
		unsigned pos = 0;

		if (!file_exists(act_node->action)) {
			list_del(&act_node->list);
			FREE(act_node);
			continue;
		}

		pos += snprintf(cmd, sizeof(cmd), "sh %s", act_node->action);
		for (int i = 0; i < act_node->idx; i++) {
			pos += snprintf(&cmd[pos], sizeof(cmd) - pos, " %s", act_node->arg[i]);
		}

		ULOG_INFO("Calling pre apply handler");
		exec_apply_handler_script(cmd);

		list_del(&act_node->list);
		FREE(act_node);
	}
}

static int bbf_config_commit_handler(struct ubus_context *ctx, struct ubus_object *obj __attribute__((unused)),
		    struct ubus_request_data *req, const char *method __attribute__((unused)),
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__MAX];
	bool monitor = false, reload = true;
	unsigned char idx = 0;
	struct list_head action_list;
	struct list_head *changed_uci;

	ULOG_INFO("Commit handler called");
	INIT_LIST_HEAD(&action_list);

	if (blobmsg_parse(bbf_config_policy, __MAX, tb, blob_data(msg), blob_len(msg))) {
		send_reply(ctx, req, "error", "Failed to parse blob");
		return -1;
	}

	struct bbf_config_async_req *async_req = calloc(1, sizeof(struct bbf_config_async_req));
	if (!async_req) {
		send_reply(ctx, req, "error", "Failed to allocate bbf config async request");
		return -1;
	}

	changed_uci = &async_req->changed_uci_list;
	INIT_LIST_HEAD(changed_uci);

	// Set internal commit to true
	g_internal_commit = true;

	async_req->ctx = ctx;

	memset(async_req->package, 0, sizeof(struct config_package) * MAX_PACKAGE_NUM);

	if (tb[SERVICES_PROTO]) {
		char *proto = blobmsg_get_string(tb[SERVICES_PROTO]);
		idx = get_idx_by_proto(proto);
		ULOG_DEBUG("Protocol index determined as %d for protocol '%s'", idx, proto);
	}

	if (tb[SERVICES_RELOAD]) {
		reload = blobmsg_get_bool(tb[SERVICES_RELOAD]);
		ULOG_DEBUG("Reload flag set to %s.", reload ? "true" : "false");
	}

	monitor = get_monitor_status(idx, tb[SERVICES_NAME]);
	if (monitor) {
		ULOG_DEBUG("Retrieving all config information before committing changes");
		fill_service_info(ctx, async_req->package, NULL, true, _get_service_list_cb);
#ifdef BBF_CONFIG_DEBUG
		show_package_tree(async_req->package);
#endif
	}


	struct blob_attr *services = tb[SERVICES_NAME];

	size_t arr_len = (services) ? blobmsg_len(services) : 0;

	// Execute pre apply handlers once (before apply handlers), based on requested services
	execute_pre_apply_handlers(services);

	if (arr_len) {
		size_t blob_data_len = blob_raw_len(services);
		if (blob_data_len) {
			async_req->services = (struct blob_attr *)calloc(1, blob_data_len);
			if (!async_req->services) {
				ULOG_ERR("Failed to allocate memory for services blob data.");
				FREE(async_req);
				send_reply(ctx, req, "error", "Memory allocation error");
				g_internal_commit = false;
				return -1;
			}

			memcpy(async_req->services, services, blob_data_len);
		}

		ULOG_INFO("Committing changes for specified services and reloading");
		reload_specified_services(ctx, idx, async_req->services, true, reload, &action_list,
					  &g_apply_handlers, changed_uci);
	} else {
		ULOG_INFO("Applying changes to dmmap UCI config");
		uci_apply_changes_dmmap(idx, true, &action_list, &g_apply_handlers);
		ULOG_INFO("Committing changes for all services and reloading");
		reload_all_services(ctx, idx, true, reload, &action_list, &g_apply_handlers, changed_uci);
	}

	struct action_node *node = NULL, *tmp = NULL;
	bool external_handler = false;

	list_for_each_entry_safe(node, tmp, &action_list, list) {
		char cmd[4096] = {0};
		unsigned pos = 0;

		ULOG_INFO("Reloading changes");

		if (!file_exists(node->action)) {
			list_del(&node->list);
			FREE(node);
			continue;
		}

		if (strcmp(node->action, DEFAULT_HANDLER_ACT) != 0)
			external_handler = true;

		pos += snprintf(cmd, sizeof(cmd), "sh %s", node->action);

		for (int i = 0; i < node->idx; i++) {
			pos += snprintf(&cmd[pos], sizeof(cmd) - pos, " %s", node->arg[i]);
		}

		ULOG_INFO("Executing apply handler: '%s'", cmd);
		exec_apply_handler_script(cmd);
		list_del(&node->list);
		FREE(node);
	}

	if (monitor && reload && !external_handler) {
		ULOG_INFO("Deferring request and setting up async completion");
		async_req->idx = idx;
		ubus_defer_request(ctx, req, &async_req->req);
		async_req->timeout.cb = complete_request_callback;
		uloop_timeout_set(&async_req->timeout, 2000);
	} else {
		async_req->idx = idx;
		ubus_defer_request(ctx, req, &async_req->req);
		async_req->timeout.cb = sync_listen_timeout;
		async_req->sync_ev.cb = sync_event_handler;

		// Set internal commit to false
		g_internal_commit = false;

		wait_for_sync_complete_event(async_req);
	}

	return 0;
}

static int bbf_config_revert_handler(struct ubus_context *ctx, struct ubus_object *obj __attribute__((unused)),
		    struct ubus_request_data *req, const char *method __attribute__((unused)),
		    struct blob_attr *msg)
{
	struct blob_attr *tb[__MAX];
	unsigned char idx = 0;
	struct list_head action_list;

	ULOG_INFO("Revert handler called");
	INIT_LIST_HEAD(&action_list);

	if (blobmsg_parse(bbf_config_policy, __MAX, tb, blob_data(msg), blob_len(msg))) {
		send_reply(ctx, req, "error", "Failed to parse blob");
		return -1;
	}

	if (tb[SERVICES_PROTO]) {
		char *proto = blobmsg_get_string(tb[SERVICES_PROTO]);
		idx = get_idx_by_proto(proto);
		ULOG_DEBUG("Protocol index determined as %d for protocol '%s'", idx, proto);
	}

	struct blob_attr *services = tb[SERVICES_NAME];

	size_t arr_len = (services) ? blobmsg_len(services) : 0;

	if (arr_len) {
		ULOG_INFO("Reverting specified services");
		reload_specified_services(ctx, idx, services, false, false, &action_list, &g_revert_handlers, NULL);
	} else {
		ULOG_INFO("Reverting changes to dmmap UCI config");
		uci_apply_changes_dmmap(idx, false, &action_list, &g_revert_handlers); // revert dmmap changes
		ULOG_INFO("Reverting all services");
		reload_all_services(ctx, idx, false, false, &action_list, &g_revert_handlers, NULL);
	}

	struct action_node *node = NULL, *tmp = NULL;
	list_for_each_entry_safe(node, tmp, &action_list, list) {
		char cmd[4096] = {0};
		unsigned pos = 0;

		ULOG_INFO("Calling external revert handlers");

		if (!file_exists(node->action)) {
			list_del(&node->list);
			FREE(node);
			continue;
		}

		pos += snprintf(cmd, sizeof(cmd), "sh %s", node->action);

		for (int i = 0; i < node->idx; i++) {
			pos += snprintf(&cmd[pos], sizeof(cmd) - pos, " %s", node->arg[i]);
		}

		exec_apply_handler_script(cmd);
		list_del(&node->list);
		FREE(node);
	}

	ULOG_INFO("Sending success response");
	send_reply(ctx, req, "status", "ok");

	ULOG_INFO("revert handler exit");

	return 0;
}

static void free_changed_uci_list(struct list_head *uci_list)
{
	struct modi_uci_node *node = NULL, *tmp = NULL;

	if (uci_list == NULL)
		return;

	list_for_each_entry_safe(node, tmp, uci_list, list) {
		list_del(&node->list);
		FREE(node->uci);
		FREE(node);
	}
}

static void receive_notify_event(struct ubus_context *ctx, struct ubus_event_handler *ev,
			  const char *type, struct blob_attr *msg)
{
	char file_path[1024] = {0};

	struct blob_attr *tb[1] = {0};
	const struct blobmsg_policy p[1] = {
		{ "config", BLOBMSG_TYPE_STRING }
	};

	blobmsg_parse(p, 1, tb, blob_data(msg), blob_len(msg));

	if (!tb[0])
		return;

	char *config = blobmsg_get_string(tb[0]);
	if (strlen(config) == 0)
		return;

	snprintf(file_path, sizeof(file_path), "/etc/config/%s", config);

	if (g_internal_commit) {
		ULOG_DEBUG("internal commit in progress, add uci in global list");
		add_changed_uci_list(&g_external_changed_uci, file_path);
		return;
	}

	// Trigger 'bbfdm.apply' event
	struct list_head uci_list;

	INIT_LIST_HEAD(&uci_list);
	add_changed_uci_list(&uci_list, file_path);

	send_bbf_apply_event(-1, &uci_list);
	free_changed_uci_list(&uci_list);

	return;
}

static const struct ubus_method bbf_config_methods[] = {
	UBUS_METHOD("commit", bbf_config_commit_handler, bbf_config_policy),
	UBUS_METHOD("revert", bbf_config_revert_handler, bbf_config_policy),
};

static struct ubus_object_type bbf_config_object_type = UBUS_OBJECT_TYPE("bbf.config", bbf_config_methods);

static struct ubus_object bbf_config_object = {
	.name = "bbf.config",
	.type = &bbf_config_object_type,
	.methods = bbf_config_methods,
	.n_methods = ARRAY_SIZE(bbf_config_methods),
};

static void usage(char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "    -l <0-4> Set the loglevel\n");
	fprintf(stderr, "    -h  Displays this help\n");
	fprintf(stderr, "\n");
}

static void load_critical_services()
{
	memset(&g_critical_bb, 0, sizeof(struct blob_buf));
	blob_buf_init(&g_critical_bb, 0);
	blobmsg_add_json_from_file(&g_critical_bb, CRITICAL_DEF_JSON);
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

static void free_apply_revert_handlers()
{
	struct applier_node *node = NULL, *tmp = NULL;

	list_for_each_entry_safe(node, tmp, &g_apply_handlers, list) {
		list_del(&node->list);
		FREE(node->file_path);
		FREE(node->action);
		FREE(node);
	}

	node = NULL;
	tmp = NULL;
	list_for_each_entry_safe(node, tmp, &g_revert_handlers, list) {
		list_del(&node->list);
		FREE(node->file_path);
		FREE(node->action);
		FREE(node);
	}

	node = NULL;
	tmp = NULL;
	list_for_each_entry_safe(node, tmp, &g_pre_apply_handlers, list) {
		list_del(&node->list);
		FREE(node->file_path);
		FREE(node->action);
		FREE(node);
	}
}

static void __load_handlers(const char *file)
{
	if (file == NULL || strlen(file) == 0)
		return;

	json_object *json_root = json_object_from_file(file);
	if (!json_root) {
		ULOG_INFO("Failed to read json file %s", file);
		return;
	}

	json_object *daemon_config = NULL;
	json_object_object_get_ex(json_root, "daemon", &daemon_config);
	if (!daemon_config) {
		ULOG_INFO("Failed to find daemon object");
		json_object_put(json_root);
		return;
	}

	const char *type[] = { "dmmap", "uci" };
	const char *method[] = { "apply_handler", "revert_handler", "pre_apply_handler" };
	struct list_head *handler_list[] = { &g_apply_handlers, &g_revert_handlers, &g_pre_apply_handlers };

	// Load apply/revert handlers
	for (int x = 0; x < ARRAY_SIZE(method); x++) {
		json_object *m_handler = NULL;
		json_object_object_get_ex(daemon_config, method[x], &m_handler);
		if (!m_handler) {
			continue;
		}

		for (int i = 0; i < ARRAY_SIZE(type); i++) {
			json_object *array = NULL;

			if (!json_object_object_get_ex(m_handler, type[i], &array) ||
			    json_object_get_type(array) != json_type_array) {
				continue;
			}

			size_t count = json_object_array_length(array);
			for (size_t j = 0; j < count; j++) {
				json_object *hndl_obj = json_object_array_get_idx(array, j);
				json_object *files = NULL, *handler = NULL;

				json_object_object_get_ex(hndl_obj, "external_handler", &handler);
				if (!handler) {
					continue;
				}

				const char *action = json_object_get_string(handler);
				if (strlen(action) == 0 || !file_exists(action)) {
					continue;
				}

				json_object_object_get_ex(hndl_obj, "file", &files);
				if (!files || json_object_get_type(files) != json_type_array) {
					continue;
				}

				size_t f_count = json_object_array_length(files);
				for (size_t k = 0; k < f_count; k++) {
					char path[1024] = {0};

					json_object *f_inst = json_object_array_get_idx(files, k);
					snprintf(path, sizeof(path), "/etc/%s/%s",
						(strcmp(type[i], "uci") == 0) ? "config" : "bbfdm/dmmap",
						json_object_get_string(f_inst));

					// check if already present
					bool exist = false;
					struct applier_node *node = NULL;
					list_for_each_entry(node, handler_list[x], list) {
						if (strcmp(node->file_path, path) == 0 && strcmp(node->action, action) == 0) {
							exist = true;
							break;
						}
					}

					if (exist == true)
						continue;

					node = (struct applier_node *)calloc(1, sizeof(struct applier_node));
					if (node == NULL) {
						ULOG_INFO("Failed to allocate memory for apply handlers");
						json_object_put(json_root);
						return;
					}

					INIT_LIST_HEAD(&node->list);
					list_add_tail(&node->list, handler_list[x]);

					node->file_path = strdup(path);
					node->action = strdup(action);
				}
			}
		}
	}

	json_object_put(json_root);
	return;
}

static void load_apply_revert_handlers()
{
	struct dirent **namelist;

	INIT_LIST_HEAD(&g_apply_handlers);
	INIT_LIST_HEAD(&g_revert_handlers);
	INIT_LIST_HEAD(&g_pre_apply_handlers);

	int num_files = scandir(BBFDM_MICROSERVICE_INPUT_PATH, &namelist, filter, compare);

	for (int i = 0; i < num_files; i++) {
		char file_path[512] = {0};

		snprintf(file_path, sizeof(file_path), "%s/%s", BBFDM_MICROSERVICE_INPUT_PATH, namelist[i]->d_name);

		if (!file_exists(file_path) || !regular_file(file_path)) {
			free(namelist[i]);
			continue;
		}

		__load_handlers(file_path);

		free(namelist[i]);
	}

	if (namelist)
		free(namelist);

	return;
}

int main(int argc, char **argv)
{
	struct ubus_event_handler ev = {
		.cb = receive_notify_event,
	};
	struct ubus_context *uctx;
	int ch, log_level = DEFAULT_LOG_LEVEL;

	while ((ch = getopt(argc, argv, "hl:")) != -1) {
		switch (ch) {
		case 'l':
			if (optarg) {
				log_level = (int)strtod(optarg, NULL);
				if (log_level < 0 || log_level > 7) {
					log_level = DEFAULT_LOG_LEVEL;
				}
			}
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			break;
		}
	}

	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "bbf.config");
	ulog_threshold(LOG_ERR + log_level);

	uctx = ubus_connect(NULL);
	if (uctx == NULL) {
		ULOG_ERR("Failed to create UBUS context");
		return -1;
	}

	uloop_init();
	ubus_add_uloop(uctx);

	load_critical_services();
	load_apply_revert_handlers();

	INIT_LIST_HEAD(&g_external_changed_uci);

	if (ubus_add_object(uctx, &bbf_config_object)) {
		ULOG_ERR("Failed to add 'bbf.config' ubus object");
		goto exit;
	}

	if (ubus_register_event_handler(uctx, &ev, "bbf.config.notify")) {
		ULOG_ERR("Failed to register 'bbf.config.notify' event handler");
		goto exit;
	}

	uloop_run();

exit:
	free_apply_revert_handlers();
	free_changed_uci_list(&g_external_changed_uci);
	blob_buf_free(&g_critical_bb);
	uloop_done();
	ubus_free(uctx);
	ulog_close();

	return 0;
}
