#ifndef FTS_SOLR_PLUGIN_H
#define FTS_SOLR_PLUGIN_H

#include "module-context.h"
#include "fts-api-private.h"

#define FTS_SOLR_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, fts_notmuch_user_module)

struct fts_notmuch_settings {
	bool debug;
};

struct fts_notmuch_user {
	union mail_user_module_context module_ctx;
	struct fts_notmuch_settings set;
};

extern const char *fts_notmuch_plugin_dependencies[];
extern struct fts_backend fts_backend_notmuch;
extern MODULE_CONTEXT_DEFINE(fts_notmuch_user_module, &mail_user_module_register);

void fts_notmuch_plugin_init(struct module *module);
void fts_notmuch_plugin_deinit(void);

#endif
