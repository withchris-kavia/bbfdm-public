#ifndef BBFDM_UBUS_H
#define BBFDM_UBUS_H

#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/list.h>

#include "libbbfdm-api/legacy/dmbbf.h"
#include "libbbfdm-api/legacy/dmplugin.h"
#include "libbbfdm-api/legacy/plugin/json_plugin.h"
#include "libbbfdm-api/legacy/plugin/dotso_plugin.h"

#define BBFDM_DEFAULT_UBUS_OBJ "bbfdm"

struct supp_module_node {
	char *service;
	struct list_head list;
};

struct bbfdm_async_req {
	struct ubus_context *ctx;
	struct ubus_request_data req;
	struct uloop_process process;
	void *result;
};

struct apply_handler_node {
	char *file_path;
	struct list_head list;
};

typedef struct bbfdm_data {
	struct ubus_context *ctx;
	struct ubus_object *obj;
	struct ubus_request_data *req;
	struct list_head *plist;
	struct dmctx bbf_ctx;
	struct blob_buf bb;
} bbfdm_data_t;

int bbfdm_ubus_register_init(struct bbfdm_context *bbfdm_ctx);
int bbfdm_ubus_register_suppress_init(struct bbfdm_context *bbfdm_ctx);
int bbfdm_ubus_register_free(struct bbfdm_context *bbfdm_ctx);

__attribute__((deprecated("Use bbfdm_ubus_register_init() instead of bbfdm_ubus_regiter_init()")))
int bbfdm_ubus_regiter_init(struct bbfdm_context *bbfdm_ctx);

__attribute__((deprecated("Use bbfdm_ubus_register_free() instead of bbfdm_ubus_regiter_free()")))
int bbfdm_ubus_regiter_free(struct bbfdm_context *bbfdm_ctx);

int bbfdm_print_data_model_schema(struct bbfdm_context *bbfdm_ctx, const enum bbfdm_type_enum type);

void bbfdm_ubus_set_service_name(struct bbfdm_context *bbfdm_ctx, const char *srv_name);

uint8_t bbfdm_ubus_get_log_level(void);
void bbfdm_ubus_set_log_level(int log_level);
void bbfdm_ubus_load_data_model(DM_MAP_OBJ *DynamicObj);
int bbfdm_refresh_references(unsigned int dm_type, const char *srv_obj_name);

#endif /* BBFDM_UBUS_H */
