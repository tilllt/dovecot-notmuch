/* Copyright (c) 2006-2012 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 John Morrissey <jwm@horde.net> */

#include "lib.h"
#include "array.h"
#include "mail-user.h"
#include "mail-storage-hooks.h"
#include "fts-notmuch-plugin.h"

#include <stdlib.h>

const char *fts_notmuch_plugin_version = DOVECOT_VERSION;

struct fts_notmuch_user_module fts_notmuch_user_module =
	MODULE_CONTEXT_INIT(&mail_user_module_register);

static int
fts_notmuch_plugin_init_settings(struct mail_user *user,
			      struct fts_notmuch_settings *set, const char *str)
{
	const char *const *tmp;

	if (str == NULL)
		str = "";

	for (tmp = t_strsplit_spaces(str, " "); *tmp != NULL; tmp++) {
		if (strcmp(*tmp, "debug") == 0) {
			set->debug = TRUE;
		} else {
			i_error("fts_notmuch: Invalid setting: %s", *tmp);
			return -1;
		}
	}
	return 0;
}

void fts_notmuch_plugin_init(struct module *module)
{
	fts_backend_register(&fts_backend_notmuch);
}

void fts_notmuch_plugin_deinit(void)
{
	fts_backend_unregister(fts_backend_notmuch.name);
}

const char *fts_notmuch_plugin_dependencies[] = { "fts", NULL };
