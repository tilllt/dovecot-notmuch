#ifndef MAIL_USER_H
#define MAIL_USER_H

#include "unichar.h"
#include "mail-storage-settings.h"

struct module;
struct mail_user;

struct mail_user_vfuncs {
	void (*deinit)(struct mail_user *user);
};

struct mail_user {
	pool_t pool;
	struct mail_user_vfuncs v, *vlast;
	int refcount;

	const char *username;
	/* don't access the home directly. It may be set lazily. */
	const char *_home;

	uid_t uid;
	gid_t gid;
	const char *service;
	const char *session_id;
	struct ip_addr *local_ip, *remote_ip;
	const char *auth_token, *auth_user;

	const struct var_expand_table *var_expand_table;
	/* If non-NULL, fail the user initialization with this error.
	   This could be set by plugins that need to fail the initialization. */
	const char *error;

	const struct setting_parser_info *set_info;
	const struct mail_user_settings *unexpanded_set;
	struct mail_user_settings *set;
	struct mail_namespace *namespaces;
	struct mail_storage *storages;
	ARRAY(const struct mail_storage_hooks *) hooks;

	struct mountpoint_list *mountpoints;
	normalizer_func_t *default_normalizer;
	/* Filled lazily by mailbox_attribute_*() when accessing attributes. */
	struct dict *_attr_dict;

	/* Module-specific contexts. See mail_storage_module_id. */
	ARRAY(union mail_user_module_context *) module_contexts;

	/* User doesn't exist (as reported by userdb lookup when looking
	   up home) */
	unsigned int nonexistent:1;
	/* Either home is set or there is no home for the user. */
	unsigned int home_looked_up:1;
	/* User is anonymous */
	unsigned int anonymous:1;
	/* This is an autocreated user (e.g. for shared namespace or
	   lda raw storage) */
	unsigned int autocreated:1;
	/* mail_user_init() has been called */
	unsigned int initialized:1;
	/* Shortcut to mail_storage_settings.mail_debug */
	unsigned int mail_debug:1;
	/* If INBOX can't be opened, log an error, but only once. */
	unsigned int inbox_open_error_logged:1;
	/* Fuzzy search works for this user (FTS enabled) */
	unsigned int fuzzy_search:1;
	/* We're running dsync */
	unsigned int dsyncing:1;
	/* Failed to create attribute dict, don't try again */
	unsigned int attr_dict_failed:1;
	/* We're deinitializing the user */
	unsigned int deinitializing:1;
	/* Enable administrator user commands for the user */
	unsigned int admin:1;
};

struct mail_user_module_register {
	unsigned int id;
};

union mail_user_module_context {
	struct mail_user_vfuncs super;
	struct mail_user_module_register *reg;
};
extern struct mail_user_module_register mail_user_module_register;
extern struct auth_master_connection *mail_user_auth_master_conn;

struct mail_user *mail_user_alloc(const char *username,
				  const struct setting_parser_info *set_info,
				  const struct mail_user_settings *set);
/* Returns -1 if settings were invalid. */
int mail_user_init(struct mail_user *user, const char **error_r);

void mail_user_ref(struct mail_user *user);
void mail_user_unref(struct mail_user **user);

/* Duplicate a mail_user. mail_user_init() and mail_namespaces_init() need to
   be called before the user is usable. */
struct mail_user *mail_user_dup(struct mail_user *user);

/* Find another user from the given user's namespaces. */
struct mail_user *mail_user_find(struct mail_user *user, const char *name);

/* Specify mail location %variable expansion data. */
void mail_user_set_vars(struct mail_user *user, const char *service,
			const struct ip_addr *local_ip,
			const struct ip_addr *remote_ip);
/* Return %variable expansion table for the user. */
const struct var_expand_table *
mail_user_var_expand_table(struct mail_user *user);

/* Specify the user's home directory. This should be called also when it's
   known that the user doesn't have a home directory to avoid the internal
   lookup. */
void mail_user_set_home(struct mail_user *user, const char *home);
/* Get the home directory for the user. Returns 1 if home directory looked up
   successfully, 0 if there is no home directory (either user doesn't exist or
   has no home directory) or -1 if lookup failed. */
int mail_user_get_home(struct mail_user *user, const char **home_r);
/* Appends path + file prefix for creating a temporary file.
   The file prefix doesn't contain any uniqueness. */
void mail_user_set_get_temp_prefix(string_t *dest,
				   const struct mail_user_settings *set);

/* Returns TRUE if plugin is loaded for the user. */
bool mail_user_is_plugin_loaded(struct mail_user *user, struct module *module);
/* If name exists in plugin_envs, return its value. */
const char *mail_user_plugin_getenv(struct mail_user *user, const char *name);
const char *mail_user_set_plugin_getenv(const struct mail_user_settings *set,
					const char *name);

/* Add more namespaces to user's namespaces. The ->next pointers may be
   changed, so the namespaces pointer will be updated to user->namespaces. */
void mail_user_add_namespace(struct mail_user *user,
			     struct mail_namespace **namespaces);
/* Drop autocreated shared namespaces that don't have any "usable" mailboxes. */
void mail_user_drop_useless_namespaces(struct mail_user *user);

/* Replace ~/ at the beginning of the path with the user's home directory. */
const char *mail_user_home_expand(struct mail_user *user, const char *path);
/* Returns 0 if ok, -1 if home directory isn't set. */
int mail_user_try_home_expand(struct mail_user *user, const char **path);
/* Returns unique user+ip identifier for anvil. */
const char *mail_user_get_anvil_userip_ident(struct mail_user *user);
/* Returns FALSE if path is in a mountpoint that should be mounted,
   but isn't mounted. In such a situation it's better to fail than to attempt
   any kind of automatic file/dir creations. error_r gives an error about which
   mountpoint should be mounted. */
bool mail_user_is_path_mounted(struct mail_user *user, const char *path,
			       const char **error_r);

/* Basically the same as mail_storage_find_class(), except automatically load
   storage plugins when needed. */
struct mail_storage *
mail_user_get_storage_class(struct mail_user *user, const char *name);

#endif
