#ifndef FTS_LUCENE_PLUGIN_H
#define FTS_LUCENE_PLUGIN_H

#include "module-context.h"
#include "mail-user.h"
#include "fts-api-private.h"

#define FTS_LUCENE_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, fts_lucene_user_module)

struct fts_lucene_settings {
	const char *textcat_conf, *textcat_dir;
};

struct fts_lucene_user {
	union mail_user_module_context module_ctx;
	struct fts_lucene_settings set;
};

extern struct fts_backend fts_backend_lucene;
extern MODULE_CONTEXT_DEFINE(fts_lucene_user_module, &mail_user_module_register);

void fts_lucene_plugin_init(struct module *module);
void fts_lucene_plugin_deinit(void);

#endif
