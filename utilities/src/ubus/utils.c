/*
 * utils.c: common function for bbf.config daemon
 *
 * Copyright (C) 2024 IOPSYS Software Solutions AB. All rights reserved.
 *
 * Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 * See LICENSE file for license related information.
 */

#include <stdarg.h>
#include <libubus.h>
#include <uci.h>

#include "utils.h"

#define DEFAULT_UBUS_TIMEOUT 5000

struct proto_args {
	const char *name;
	const char *config_savedir;
	const char *dmmap_savedir;
	unsigned char index;
};

static struct proto_args supported_protocols[] = {
		{
				"both", "/tmp/bbfdm/.bbfdm/config/", "/tmp/bbfdm/.bbfdm/dmmap/", 0
		},
		{
				"cwmp", "/tmp/bbfdm/.cwmp/config/", "/tmp/bbfdm/.cwmp/dmmap/", 1
		},
		{
				"usp", "/tmp/bbfdm/.usp/config/", "/tmp/bbfdm/.usp/dmmap/", 2
		},
};

unsigned char get_idx_by_proto(const char *proto)
{
	for (int i = 0; i < ARRAY_SIZE(supported_protocols); i++) {
		if (strcmp(supported_protocols[i].name, proto) == 0)
			return supported_protocols[i].index;
	}

	return 0;
}

const char *get_proto_conf_savedir_by_idx(int idx)
{
	if (idx < ARRAY_SIZE(supported_protocols)) {
		return supported_protocols[idx].config_savedir;
	}

	return "";
}

const char *get_proto_dmmap_savedir_by_idx(int idx)
{
	if (idx < ARRAY_SIZE(supported_protocols)) {
		return supported_protocols[idx].dmmap_savedir;
	}

	return "";
}

const char *get_proto_name_by_idx(int idx)
{
	if (idx < ARRAY_SIZE(supported_protocols)) {
		return supported_protocols[idx].name;
	}

	return "";
}

void strncpyt(char *dst, const char *src, size_t n)
{
	if (dst == NULL || src == NULL)
		return;

        if (n > 1) {
                strncpy(dst, src, n - 1);
                dst[n - 1] = 0;
        }
}

bool file_exists(const char *path)
{
	struct stat buffer;

	if (!path)
		return false;

	return stat(path, &buffer) == 0;
}

static bool is_wifi_configs(const char *config_name, uint32_t *wifi_config_flags)
{
	if (!config_name && !wifi_config_flags)
		return false;

	if (strcmp(config_name, "wireless") == 0) {
		*wifi_config_flags |= WIRELESS_CONFIG;
		return true;
	}

	if (strcmp(config_name, "mapcontroller") == 0) {
		*wifi_config_flags |= MAPCONTROLLER_CONFIG;
		return true;
	}

	return false;
}

int bbf_config_call(struct ubus_context *ctx, const char *object, const char *method, struct blob_buf *data, ubus_data_handler_t callback, void *arg)
{
	int fault = 0;
	uint32_t id;

	if (!ctx) {
		ULOG_ERR("Failed to execute 'bbf_config_call': 'ctx' is NULL.");
		return -1;
	}

	fault = ubus_lookup_id(ctx, object, &id);
	if (fault) {
		ULOG_ERR("Failed to find UBUS object ID for '%s'. Error code: %d", object, fault);
		return -1;
	}

	fault = ubus_invoke(ctx, id, method, data ? data->head : NULL, callback, arg, DEFAULT_UBUS_TIMEOUT);
	if (fault) {
		ULOG_ERR("UBUS invoke failed for method '%s' on object '%s'. Error code: %d", method, object, fault);
		return -1;
	}

	return 0;
}

static void reload_service(struct ubus_context *ctx, const char *config_name, bool is_commit)
{
	struct blob_buf bb = {0};

	if (!ctx || !config_name) {
		ULOG_ERR("Failed to reload service: 'ctx' or 'config_name' is NULL");
		return;
	}

	memset(&bb, 0, sizeof(struct blob_buf));

	blob_buf_init(&bb, 0);

	blobmsg_add_string(&bb, "config", config_name);

	int result = bbf_config_call(ctx, "uci", (is_commit) ? "commit" : "revert", &bb, NULL, NULL);
	if (result != 0) {
		ULOG_ERR("Failed to %s configuration '%s'", (is_commit ? "commit" : "revert"), config_name);
	} else {
		ULOG_DEBUG("Successfully executed %s on configuration '%s'.", (is_commit ? "commit" : "revert"), config_name);
	}

	blob_buf_free(&bb);
}


void reload_specified_services(struct ubus_context *ctx, int idx, struct blob_attr *services,
		bool is_commit, bool reload, uint32_t *wifi_config_flags)
{
	struct uci_context *uci_ctx = NULL;
	struct blob_attr *service = NULL;
	size_t rem = 0;

	uci_ctx = uci_alloc_context();
	if (!uci_ctx) {
		ULOG_ERR("Failed to allocate UCI context");
		return;
	}

	ULOG_DEBUG("Processing services list...");
	blobmsg_for_each_attr(service, services, rem) {
		struct uci_ptr ptr = {0};
		char conf_dir[64] = {0};
		char save_dir[64] = {0};
		char package[64] = {0};
		bool is_dmmap = false;

		char *config_name = blobmsg_get_string(service);
		if (strncmp(CONFIG_CONFDIR, config_name, strlen(CONFIG_CONFDIR)) == 0) {
			/* standard uci path received */
			snprintf(conf_dir, sizeof(conf_dir), "%s", CONFIG_CONFDIR);
			snprintf(save_dir, sizeof(save_dir), "%s", get_proto_conf_savedir_by_idx(idx));
			snprintf(package, sizeof(package), "%s", config_name + strlen(CONFIG_CONFDIR));
		} else if (strncmp(DMMAP_CONFDIR, config_name, strlen(DMMAP_CONFDIR)) == 0) {
			/* dmmap uci path received */
			snprintf(conf_dir, sizeof(conf_dir), "%s", DMMAP_CONFDIR);
			snprintf(save_dir, sizeof(save_dir), "%s", get_proto_dmmap_savedir_by_idx(idx));
			snprintf(package, sizeof(package), "%s", config_name + strlen(DMMAP_CONFDIR));
			is_dmmap = true;
		} else {
			/* no path default to standard uci */
			snprintf(conf_dir, sizeof(conf_dir), "%s", CONFIG_CONFDIR);
			snprintf(save_dir, sizeof(save_dir), "%s", get_proto_conf_savedir_by_idx(idx));
			snprintf(package, sizeof(package), "%s", config_name);
		}

		ULOG_DEBUG("Setting UCI configuration directory to '%s'", conf_dir);
		uci_set_confdir(uci_ctx, conf_dir);

		ULOG_DEBUG("Setting UCI save directory to '%s'", save_dir);
		uci_set_savedir(uci_ctx, save_dir);

		ULOG_DEBUG("Looking up UCI configuration for service '%s'", config_name);

		if (uci_lookup_ptr(uci_ctx, &ptr, package, true) != UCI_OK) {
			ULOG_ERR("Failed to lookup UCI pointer for service '%s'. Skipping", config_name);
			continue;
		}

		if (is_commit) {
			ULOG_DEBUG("Committing UCI changes for service '%s'", config_name);
			if (uci_commit(uci_ctx, &ptr.p, false) != UCI_OK) {
				ULOG_ERR("Failed to commit UCI changes for service '%s'", config_name);
				continue;
			}
		} else {
			ULOG_DEBUG("Reverting UCI changes for service '%s'", config_name);
			if (uci_revert(uci_ctx, &ptr) != UCI_OK) {
				ULOG_ERR("Failed to revert UCI changes for service '%s'", config_name);
				continue;
			}
		}

		if (reload && !is_dmmap) {
			if (is_wifi_configs(package, wifi_config_flags))
				continue;

			ULOG_INFO("Reloading service '%s'", package);
			reload_service(ctx, package, is_commit);
		}
	}

	ULOG_DEBUG("Freeing UCI context");
	uci_free_context(uci_ctx);
}

void reload_all_services(struct ubus_context *ctx, int idx, bool is_commit,
		bool reload, uint32_t *wifi_config_flags)
{
	struct uci_context *uci_ctx = NULL;
	char **configs = NULL, **p = NULL;

	uci_ctx = uci_alloc_context();
	if (!uci_ctx) {
		ULOG_ERR("Failed to allocate UCI context");
		return;
	}

	ULOG_DEBUG("Setting UCI configuration directory to '%s'", CONFIG_CONFDIR);
	uci_set_confdir(uci_ctx, CONFIG_CONFDIR);

	const char *save_dir = get_proto_conf_savedir_by_idx(idx);
	ULOG_DEBUG("Setting UCI save directory to '%s'", save_dir);
	uci_set_savedir(uci_ctx, save_dir);

	if (uci_list_configs(uci_ctx, &configs) != UCI_OK) {
		ULOG_ERR("Failed to list UCI configurations");
		goto exit;
	}

	ULOG_DEBUG("Processing all configurations...");
	for (p = configs; p && *p; p++) {
		struct uci_ptr ptr = {0};

		ULOG_DEBUG("Looking up UCI configuration for '%s'", *p);

		if (uci_lookup_ptr(uci_ctx, &ptr, *p, true) != UCI_OK) {
			ULOG_ERR("Failed to lookup UCI pointer for config '%s'. Skipping", *p);
			continue;
		}

		if (uci_list_empty(&ptr.p->saved_delta)) {
			ULOG_DEBUG("No changes detected in config '%s'. Skipping", *p);
			continue;
		}

		if (is_commit) {
			ULOG_DEBUG("Committing UCI changes for config '%s'", *p);
			if (uci_commit(uci_ctx, &ptr.p, false) != UCI_OK) {
				ULOG_ERR("Failed to commit changes for config '%s'", *p);
				continue;
			}
		} else {
			ULOG_DEBUG("Reverting UCI changes for config '%s'", *p);
			if (uci_revert(uci_ctx, &ptr) != UCI_OK) {
				ULOG_ERR("Failed to revert changes for config '%s'", *p);
				continue;
			}
		}

		if (reload) {
			if (is_wifi_configs(*p, wifi_config_flags))
				continue;

			ULOG_INFO("Reloading service for config '%s'", *p);
			reload_service(ctx, *p, is_commit);
		}
	}

	FREE(configs);

exit:
	uci_free_context(uci_ctx);
}

void wifi_reload_handler_script(uint32_t wifi_config_flags)
{
#define WIFI_CONFIG_RELOAD_SCRIPT "/etc/wifidmd/bbf_config_reload.sh"
	char cmd[512] = {0};

	if (!file_exists(WIFI_CONFIG_RELOAD_SCRIPT))
		return;

	snprintf(cmd, sizeof(cmd), "sh %s '{\"wireless\":\"%d\",\"mapcontroller\":\"%d\"}'",
			WIFI_CONFIG_RELOAD_SCRIPT,
			wifi_config_flags & WIRELESS_CONFIG ? 1 : 0,
			wifi_config_flags & MAPCONTROLLER_CONFIG ? 1 : 0);

	FILE *pp = popen(cmd, "r"); // flawfinder: ignore
	if (pp) {
		pclose(pp);
	}
}

void uci_apply_changes(const char *conf_dir, int idx, bool is_commit)
{
	struct uci_context *uci_ctx = NULL;
	char **configs = NULL, **p = NULL;
	char save_dir[128] = {0};

	uci_ctx = uci_alloc_context();
	if (!uci_ctx) {
		ULOG_ERR("Failed to allocate UCI context");
		return;
	}

	if (conf_dir) {
		ULOG_DEBUG("Setting UCI configuration directory to '%s'", conf_dir);
		uci_set_confdir(uci_ctx, conf_dir);

		if (strcmp(conf_dir, DMMAP_CONFDIR) == 0) {
			snprintf(save_dir, sizeof(save_dir), "%s", get_proto_dmmap_savedir_by_idx(idx));
		} else if(strcmp(conf_dir, CONFIG_CONFDIR) == 0) {
			snprintf(save_dir, sizeof(save_dir), "%s", get_proto_conf_savedir_by_idx(idx));
		}
	}

	ULOG_DEBUG("Setting UCI save directory to '%s'", save_dir);
	uci_set_savedir(uci_ctx, save_dir);

	if (uci_list_configs(uci_ctx, &configs) != UCI_OK) {
		ULOG_ERR("Failed to list UCI configurations");
		goto exit;
	}

	ULOG_DEBUG("Applying changes to all configurations...");
	for (p = configs; p && *p; p++) {
		struct uci_ptr ptr = {0};

		ULOG_DEBUG("Looking up UCI configuration for '%s'", *p);

		if (uci_lookup_ptr(uci_ctx, &ptr, *p, true) != UCI_OK) {
			ULOG_ERR("Failed to lookup UCI pointer for config '%s'. Skipping", *p);
			continue;
		}

		if (is_commit) {
			ULOG_DEBUG("Committing changes for config '%s'", *p);
			if (uci_commit(uci_ctx, &ptr.p, false) != UCI_OK) {
				ULOG_ERR("Failed to commit changes for config '%s'", *p);
				continue;
			}
		} else {
			ULOG_DEBUG("Reverting changes for config '%s'", *p);
			if (uci_revert(uci_ctx, &ptr) != UCI_OK) {
				ULOG_ERR("Failed to revert changes for config '%s'", *p);
				continue;
			}
		}
	}

	FREE(configs);

exit:
	uci_free_context(uci_ctx);
}
