/*
 * utils.c: common function for bbf.config daemon
 *
 * Copyright (C) 2024 IOPSYS Software Solutions AB. All rights reserved.
 *
 * Author: Amin Ben Romdhane <amin.benromdhane@iopsys.eu>
 *
 * See LICENSE file for license related information.
 */

#ifndef __UTILS_H__
#define __UTILS_H__

#include <libubox/ulog.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef FREE
#define FREE(x) do { if(x) {free(x); x = NULL;} } while (0)
#endif

#ifndef ULOG_DEBUG
#define ULOG_DEBUG(fmt, ...) ulog(LOG_DEBUG, fmt, ## __VA_ARGS__)
#endif

#define CONFIG_CONFDIR "/etc/config/"
#define DMMAP_CONFDIR "/etc/bbfdm/dmmap/"

enum wifi_config_flags_enum {
	WIRELESS_CONFIG = 1,
	MAPCONTROLLER_CONFIG = 1<<1,
};

void strncpyt(char *dst, const char *src, size_t n);

int bbf_config_call(struct ubus_context *ctx, const char *object, const char *method, struct blob_buf *data, ubus_data_handler_t callback, void *arg);

void reload_specified_services(struct ubus_context *ctx, int idx, struct blob_attr *services,
		bool is_commit, bool reload, uint32_t *wifi_config_flags);

void reload_all_services(struct ubus_context *ctx, int idx, bool is_commit,
		bool reload, uint32_t *wifi_config_flags);

void wifi_reload_handler_script(uint32_t wifi_config_flags);

void uci_apply_changes(const char *conf_dir, int idx, bool is_commit);

unsigned char get_idx_by_proto(const char *proto);
const char *get_proto_conf_savedir_by_idx(int idx);
const char *get_proto_dmmap_savedir_by_idx(int idx);
const char *get_proto_name_by_idx(int idx);

#endif //__UTILS_H__
