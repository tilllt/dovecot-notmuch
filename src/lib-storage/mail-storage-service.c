/* Copyright (c) 2009-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hostpid.h"
#include "module-dir.h"
#include "restrict-access.h"
#include "eacces-error.h"
#include "ipwd.h"
#include "str.h"
#include "var-expand.h"
#include "dict.h"
#include "settings-parser.h"
#include "auth-master.h"
#include "master-service-private.h"
#include "master-service-settings.h"
#include "master-service-settings-cache.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-service.h"

#include <stdlib.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

/* If time moves backwards more than this, kill ourself instead of sleeping. */
#define MAX_TIME_BACKWARDS_SLEEP 5
#define MAX_NOWARN_FORWARD_SECS 10

#define ERRSTR_INVALID_USER_SETTINGS \
	"Invalid user settings. Refer to server log for more information."

struct mail_storage_service_privileges {
	uid_t uid;
	gid_t gid;
	const char *uid_source, *gid_source;

	const char *home;
	const char *chroot;
};

struct mail_storage_service_ctx {
	pool_t pool;
	struct master_service *service;
	const char *default_log_prefix;

	struct auth_master_connection *conn, *iter_conn;
	struct auth_master_user_list_ctx *auth_list;
	const struct setting_parser_info **set_roots;
	enum mail_storage_service_flags flags;

	const char *set_cache_module, *set_cache_service;
	struct master_service_settings_cache *set_cache;

	pool_t userdb_next_pool;
	const char *const **userdb_next_fieldsp;

	unsigned int debug:1;
	unsigned int log_initialized:1;
	unsigned int config_permission_denied:1;
};

struct mail_storage_service_user {
	pool_t pool;
	struct mail_storage_service_ctx *service_ctx;
	struct mail_storage_service_input input;
	enum mail_storage_service_flags flags;

	struct ioloop_context *ioloop_ctx;
	const char *log_prefix, *auth_token, *auth_user;

	const char *system_groups_user, *uid_source, *gid_source;
	const struct mail_user_settings *user_set;
	const struct setting_parser_info *user_info;
	struct setting_parser_context *set_parser;

	unsigned int anonymous:1;
	unsigned int admin:1;
};

struct module *mail_storage_service_modules = NULL;

static bool
mail_user_set_get_mail_debug(const struct setting_parser_info *user_info,
			     const struct mail_user_settings *user_set)
{
	const struct mail_storage_settings *mail_set;

	mail_set = mail_user_set_get_driver_settings(user_info, user_set,
						MAIL_STORAGE_SET_DRIVER_NAME);
	return mail_set->mail_debug;
}

static void set_keyval(struct mail_storage_service_ctx *ctx,
		       struct mail_storage_service_user *user,
		       const char *key, const char *value)
{
	struct setting_parser_context *set_parser = user->set_parser;
	const char *str;

	if (master_service_set_has_config_override(ctx->service, key)) {
		/* this setting was already overridden with -o parameter */
		if (mail_user_set_get_mail_debug(user->user_info,
						 user->user_set)) {
			i_debug("Ignoring overridden (-o) userdb setting: %s",
				key);
		}
		return;
	}

	str = t_strconcat(key, "=", value, NULL);
	if (settings_parse_line(set_parser, str) < 0) {
		i_fatal("Invalid userdb input '%s': %s", str,
			settings_parser_get_error(set_parser));
	}
}

static int set_line(struct mail_storage_service_ctx *ctx,
		    struct mail_storage_service_user *user,
		    const char *line)
{
	struct setting_parser_context *set_parser = user->set_parser;
	bool mail_debug;
	const char *key, *orig_key, *append_value = NULL;
	unsigned int len;
	int ret;

	mail_debug = mail_user_set_get_mail_debug(user->user_info,
						  user->user_set);
	if (strchr(line, '=') == NULL)
		line = t_strconcat(line, "=yes", NULL);
	orig_key = key = t_strcut(line, '=');

	len = strlen(key);
	if (len > 0 && key[len-1] == '+') {
		/* key+=value */
		append_value = line + len + 1;
		key = t_strndup(key, len-1);
	}

	if (!settings_parse_is_valid_key(set_parser, key)) {
		/* assume it's a plugin setting */
		key = t_strconcat("plugin/", key, NULL);
		line = t_strconcat("plugin/", line, NULL);
	}

	if (master_service_set_has_config_override(ctx->service, key)) {
		/* this setting was already overridden with -o parameter */
		if (mail_debug) {
			i_debug("Ignoring overridden (-o) userdb setting: %s",
				key);
		}
		return 1;
	}

	if (append_value != NULL) {
		const void *value;
		enum setting_type type;

		value = settings_parse_get_value(set_parser, key, &type);
		if (type == SET_STR) {
			const char *const *strp = value;

			line = t_strdup_printf("%s=%s%s",
					       key, *strp, append_value);
		} else {
			i_error("Ignoring %s userdb setting. "
				"'+' can only be used for strings.", orig_key);
		}
	}

	ret = settings_parse_line(set_parser, line);
	if (mail_debug && ret >= 0) {
		if (strstr(key, "pass") != NULL) {
			/* possibly a password field (e.g. imapc_password).
			   hide the value. */
			line = t_strconcat(key, "=<hidden>", NULL);
		}
		i_debug(ret == 0 ?
			"Unknown userdb setting: %s" :
			"Added userdb setting: %s", line);
	}
	return ret;
}

static bool validate_chroot(const struct mail_user_settings *user_set,
			    const char *dir)
{
	const char *const *chroot_dirs;

	if (*dir == '\0')
		return FALSE;

	if (*user_set->valid_chroot_dirs == '\0')
		return FALSE;

	chroot_dirs = t_strsplit(user_set->valid_chroot_dirs, ":");
	while (*chroot_dirs != NULL) {
		if (**chroot_dirs != '\0' &&
		    strncmp(dir, *chroot_dirs, strlen(*chroot_dirs)) == 0)
			return TRUE;
		chroot_dirs++;
	}
	return FALSE;
}

static int
user_reply_handle(struct mail_storage_service_ctx *ctx,
		  struct mail_storage_service_user *user,
		  const struct auth_user_reply *reply,
		  const char **error_r)
{
	const char *home = reply->home;
	const char *chroot = reply->chroot;
	const char *const *str, *line, *p;
	unsigned int i, count;
	int ret = 0;

	if (reply->uid != (uid_t)-1) {
		if (reply->uid == 0) {
			*error_r = "userdb returned 0 as uid";
			return -1;
		}
		user->uid_source = "userdb lookup";
		set_keyval(ctx, user, "mail_uid", dec2str(reply->uid));
	}
	if (reply->gid != (uid_t)-1) {
		user->gid_source = "userdb lookup";
		set_keyval(ctx, user, "mail_gid", dec2str(reply->gid));
	}

	if (home != NULL && chroot == NULL &&
	    *user->user_set->valid_chroot_dirs != '\0' &&
	    (p = strstr(home, "/./")) != NULL) {
		/* wu-ftpd like <chroot>/./<home> - check only if there's even
		   a possibility of using them (non-empty valid_chroot_dirs) */
		chroot = t_strdup_until(home, p);
		home = p + 2;
	}

	if (home != NULL)
		set_keyval(ctx, user, "mail_home", home);

	if (chroot != NULL) {
		if (!validate_chroot(user->user_set, chroot)) {
			*error_r = t_strdup_printf(
				"userdb returned invalid chroot directory: %s "
				"(see valid_chroot_dirs setting)", chroot);
			return -1;
		}
		set_keyval(ctx, user, "mail_chroot", chroot);
	}

	user->anonymous = reply->anonymous;

	str = array_get(&reply->extra_fields, &count);
	for (i = 0; i < count; i++) {
		line = str[i];
		if (strncmp(line, "system_groups_user=", 19) == 0) {
			user->system_groups_user =
				p_strdup(user->pool, line + 19);
		} else if (strncmp(line, "nice=", 5) == 0) {
#ifdef HAVE_SETPRIORITY
			int n = atoi(line + 5);

			if (n != 0) {
				if (setpriority(PRIO_PROCESS, 0, n) < 0)
					i_error("setpriority(%d) failed: %m", n);
			}
#endif
		} else if (strncmp(line, "auth_token=", 11) == 0) {
			user->auth_token = p_strdup(user->pool, line+11);
		} else if (strncmp(line, "auth_user=", 10) == 0) {
			user->auth_user = p_strdup(user->pool, line+10);
		} else if (strncmp(line, "admin=", 6) == 0) {
			user->admin = line[6] == 'y' || line[6] == 'Y' ||
				line[6] == '1';
		} else T_BEGIN {
			ret = set_line(ctx, user, line);
		} T_END;
		if (ret < 0)
			break;
	}

	if (ret < 0) {
		*error_r = t_strdup_printf("Invalid userdb input '%s': %s",
			str[i], settings_parser_get_error(user->set_parser));
	}
	return ret;
}

static int
service_auth_userdb_lookup(struct mail_storage_service_ctx *ctx,
			   const struct mail_storage_service_input *input,
			   pool_t pool, const char **user,
			   const char *const **fields_r,
			   const char **error_r)
{
	struct auth_user_info info;
	const char *new_username;
	int ret;

	memset(&info, 0, sizeof(info));
	info.service = input->service != NULL ? input->service :
		ctx->service->name;
	info.local_ip = input->local_ip;
	info.remote_ip = input->remote_ip;
	info.local_port = input->local_port;
	info.remote_port = input->remote_port;

	ret = auth_master_user_lookup(ctx->conn, *user, &info, pool,
				      &new_username, fields_r);
	if (ret > 0) {
		if (strcmp(*user, new_username) != 0) {
			if (ctx->debug)
				i_debug("changed username to %s", new_username);
			*user = t_strdup(new_username);
		}
		*user = new_username;
	} else if (ret == 0)
		*error_r = "Unknown user";
	else if (**fields_r != NULL) {
		*error_r = t_strdup(**fields_r);
		ret = -2;
	} else {
		*error_r = MAIL_ERRSTR_CRITICAL_MSG;
	}
	return ret;
}

static bool parse_uid(const char *str, uid_t *uid_r, const char **error_r)
{
	struct passwd pw;

	if (str_to_uid(str, uid_r) == 0)
		return TRUE;

	switch (i_getpwnam(str, &pw)) {
	case -1:
		*error_r = t_strdup_printf("getpwnam(%s) failed: %m", str);
		return FALSE;
	case 0:
		*error_r = t_strconcat("Unknown UNIX UID user: ", str, NULL);
		return FALSE;
	default:
		*uid_r = pw.pw_uid;
		return TRUE;
	}
}

static bool parse_gid(const char *str, gid_t *gid_r, const char **error_r)
{
	struct group gr;

	if (str_to_gid(str, gid_r) == 0)
		return TRUE;

	switch (i_getgrnam(str, &gr)) {
	case -1:
		*error_r = t_strdup_printf("getgrnam(%s) failed: %m", str);
		return FALSE;
	case 0:
		*error_r = t_strconcat("Unknown UNIX GID group: ", str, NULL);
		return FALSE;
	default:
		*gid_r = gr.gr_gid;
		return TRUE;
	}
}

static const struct var_expand_table *
get_var_expand_table(struct master_service *service,
		     struct mail_storage_service_user *user,
		     struct mail_storage_service_input *input,
		     struct mail_storage_service_privileges *priv)
{
	static struct var_expand_table static_tab[] = {
		{ 'u', NULL, "user" },
		{ 'n', NULL, "username" },
		{ 'd', NULL, "domain" },
		{ 's', NULL, "service" },
		{ 'l', NULL, "lip" },
		{ 'r', NULL, "rip" },
		{ 'p', NULL, "pid" },
		{ 'i', NULL, "uid" },
		{ '\0', NULL, "gid" },
		{ '\0', NULL, "session" },
		{ '\0', NULL, "auth_user" },
		{ '\0', NULL, "auth_username" },
		{ '\0', NULL, "auth_domain" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = input->username;
	tab[1].value = t_strcut(input->username, '@');
	tab[2].value = strchr(input->username, '@');
	if (tab[2].value != NULL) tab[2].value++;
	tab[3].value = service->name;
	tab[4].value = net_ip2addr(&input->local_ip);
	tab[5].value = net_ip2addr(&input->remote_ip);
	tab[6].value = my_pid;
	tab[7].value = dec2str(priv->uid == (uid_t)-1 ? geteuid() : priv->uid);
	tab[8].value = dec2str(priv->gid == (gid_t)-1 ? getegid() : priv->gid);
	tab[9].value = input->session_id;
	if (user == NULL || user->auth_user == NULL) {
		tab[10].value = tab[0].value;
		tab[11].value = tab[1].value;
		tab[12].value = tab[2].value;
	} else {
		tab[10].value = user->auth_user;
		tab[11].value = t_strcut(user->auth_user, '@');
		tab[12].value = strchr(user->auth_user, '@');
	}
	return tab;
}

const struct var_expand_table *
mail_storage_service_get_var_expand_table(struct mail_storage_service_ctx *ctx,
					  struct mail_storage_service_input *input)
{
	struct mail_storage_service_privileges priv;

	memset(&priv, 0, sizeof(priv));
	priv.uid = (uid_t)-1;
	priv.gid = (gid_t)-1;
	return get_var_expand_table(ctx->service, NULL, input, &priv);
}

static const char *
user_expand_varstr(struct master_service *service,
		   struct mail_storage_service_user *user,
		   struct mail_storage_service_privileges *priv,
		   const char *str)
{
	string_t *ret;

	if (*str == SETTING_STRVAR_EXPANDED[0])
		return str + 1;

	i_assert(*str == SETTING_STRVAR_UNEXPANDED[0]);

	ret = t_str_new(256);
	var_expand(ret, str + 1,
		   get_var_expand_table(service, user, &user->input, priv));
	return str_c(ret);
}

static int
service_parse_privileges(struct mail_storage_service_ctx *ctx,
			 struct mail_storage_service_user *user,
			 struct mail_storage_service_privileges *priv_r,
			 const char **error_r)
{
	const struct mail_user_settings *set = user->user_set;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;

	memset(priv_r, 0, sizeof(*priv_r));
	if (*set->mail_uid != '\0') {
		if (!parse_uid(set->mail_uid, &uid, error_r)) {
			*error_r = t_strdup_printf("%s (from %s)", *error_r,
						   user->uid_source);
			return -1;
		}
		if (uid < (uid_t)set->first_valid_uid ||
		    (set->last_valid_uid != 0 &&
		     uid > (uid_t)set->last_valid_uid)) {
			*error_r = t_strdup_printf(
				"Mail access for users with UID %s not permitted "
				"(see first_valid_uid in config file, uid from %s).",
				dec2str(uid), user->uid_source);
			return -1;
		}
	}
	priv_r->uid = uid;
	priv_r->uid_source = user->uid_source;

	if (*set->mail_gid != '\0') {
		if (!parse_gid(set->mail_gid, &gid, error_r)) {
			*error_r = t_strdup_printf("%s (from %s)", *error_r,
						   user->gid_source);
			return -1;
		}
		if (gid < (gid_t)set->first_valid_gid ||
		    (set->last_valid_gid != 0 &&
		     gid > (gid_t)set->last_valid_gid)) {
			*error_r = t_strdup_printf(
				"Mail access for users with GID %s not permitted "
				"(see first_valid_gid in config file, gid from %s).",
				dec2str(gid), user->gid_source);
			return -1;
		}
	}
	priv_r->gid = gid;
	priv_r->gid_source = user->gid_source;

	/* variable strings are expanded in mail_user_init(),
	   but we need the home and chroot sooner so do them separately here. */
	priv_r->home = user_expand_varstr(ctx->service, user, priv_r,
					  user->user_set->mail_home);
	priv_r->chroot = user_expand_varstr(ctx->service, user, priv_r,
					    user->user_set->mail_chroot);
	return 0;
}

static void mail_storage_service_seteuid_root(void)
{
	if (seteuid(0) < 0) {
		i_fatal("mail-storage-service: "
			"Failed to restore temporarily dropped root privileges: "
			"seteuid(0) failed: %m");
	}
}

static int
service_drop_privileges(struct mail_storage_service_user *user,
			struct mail_storage_service_privileges *priv,
			bool disallow_root, bool keep_setuid_root,
			bool setenv_only, const char **error_r)
{
	const struct mail_user_settings *set = user->user_set;
	struct restrict_access_settings rset;
	uid_t current_euid, setuid_uid = 0;
	const char *cur_chroot, *error;

	current_euid = geteuid();
	restrict_access_init(&rset);
	restrict_access_get_env(&rset);
	if (priv->uid != (uid_t)-1) {
		rset.uid = priv->uid;
		rset.uid_source = priv->uid_source;
	} else if (rset.uid == (uid_t)-1 &&
		 disallow_root && current_euid == 0) {
		*error_r = "User is missing UID (see mail_uid setting)";
		return -1;
	}
	if (priv->gid != (gid_t)-1) {
		rset.gid = priv->gid;
		rset.gid_source = priv->gid_source;
	} else if (rset.gid == (gid_t)-1 && disallow_root &&
		   set->first_valid_gid > 0 && getegid() == 0) {
		*error_r = "User is missing GID (see mail_gid setting)";
		return -1;
	}
	if (*set->mail_privileged_group != '\0') {
		if (!parse_gid(set->mail_privileged_group, &rset.privileged_gid,
			       &error)) {
			*error_r = t_strdup_printf(
				"%s (in mail_privileged_group setting)", error);
			return -1;
		}
	}
	if (*set->mail_access_groups != '\0') {
		rset.extra_groups = t_strconcat(set->mail_access_groups, ",",
						rset.extra_groups, NULL);
	}

	rset.first_valid_gid = set->first_valid_gid;
	rset.last_valid_gid = set->last_valid_gid;
	rset.chroot_dir = *priv->chroot == '\0' ? NULL : priv->chroot;
	rset.system_groups_user = user->system_groups_user;

	cur_chroot = restrict_access_get_current_chroot();
	if (cur_chroot != NULL) {
		/* we're already chrooted. make sure the chroots are equal. */
		if (rset.chroot_dir == NULL) {
			*error_r = "Process is already chrooted, "
				"can't un-chroot for this user";
			return -1;
		}
		if (strcmp(rset.chroot_dir, cur_chroot) != 0) {
			*error_r = t_strdup_printf(
				"Process is already chrooted to %s, "
				"can't chroot to %s", cur_chroot, priv->chroot);
			return -1;
		}
		/* chrooting to same directory where we're already chrooted */
		rset.chroot_dir = NULL;
	}

	if (disallow_root &&
	    (rset.uid == 0 || (rset.uid == (uid_t)-1 && current_euid == 0))) {
		*error_r = "Mail access not allowed for root";
		return -1;
	}

	if (keep_setuid_root) {
		if (current_euid != rset.uid) {
			if (current_euid != 0) {
				/* we're changing the UID,
				   switch back to root first */
				mail_storage_service_seteuid_root();
			}
			setuid_uid = rset.uid;
		}
		rset.uid = (uid_t)-1;
		disallow_root = FALSE;
	}
	if (!setenv_only) {
		restrict_access(&rset, *priv->home == '\0' ? NULL : priv->home,
				disallow_root);
	} else {
		restrict_access_set_env(&rset);
	}
	if (setuid_uid != 0 && !setenv_only) {
		if (seteuid(setuid_uid) < 0)
			i_fatal("mail-storage-service: seteuid(%s) failed: %m",
				dec2str(setuid_uid));
	}
	return 0;
}

static int
mail_storage_service_init_post(struct mail_storage_service_ctx *ctx,
			       struct mail_storage_service_user *user,
			       struct mail_storage_service_privileges *priv,
			       struct mail_user **mail_user_r,
			       const char **error_r)
{
	const struct mail_storage_settings *mail_set;
	const char *home = priv->home;
	struct mail_user *mail_user;

	/* NOTE: if more user initialization is added, add it also to
	   mail_user_dup() */
	mail_user = mail_user_alloc(user->input.username, user->user_info,
				    user->user_set);
	mail_user_set_home(mail_user, *home == '\0' ? NULL : home);
	mail_user_set_vars(mail_user, ctx->service->name,
			   &user->input.local_ip, &user->input.remote_ip);
	mail_user->uid = priv->uid == (uid_t)-1 ? geteuid() : priv->uid;
	mail_user->gid = priv->gid == (gid_t)-1 ? getegid() : priv->gid;
	mail_user->anonymous = user->anonymous;
	mail_user->admin = user->admin;
	mail_user->auth_token = p_strdup(mail_user->pool, user->auth_token);
	mail_user->auth_user = p_strdup(mail_user->pool, user->auth_user);
	mail_user->session_id =
		p_strdup(mail_user->pool, user->input.session_id);
	
	mail_set = mail_user_set_get_storage_set(mail_user);

	if (mail_set->mail_debug) {
		string_t *str = t_str_new(64);

		str_printfa(str, "Effective uid=%s, gid=%s, home=%s",
			    dec2str(geteuid()), dec2str(getegid()), home);
		if (*priv->chroot != '\0')
			str_printfa(str, ", chroot=%s", priv->chroot);
		i_debug("%s", str_c(str));
	}

	if ((user->flags & MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP) != 0 &&
	    (user->flags & MAIL_STORAGE_SERVICE_FLAG_ENABLE_CORE_DUMPS) == 0) {
		/* we don't want to write core files to any users' home
		   directories since they could contain information about other
		   users' mails as well. so do no chdiring to home. */
	} else if (*home != '\0' &&
		   (user->flags & MAIL_STORAGE_SERVICE_FLAG_NO_CHDIR) == 0) {
		/* If possible chdir to home directory, so that core file
		   could be written in case we crash. */
		if (chdir(home) < 0) {
			if (errno == EACCES) {
				i_error("%s", eacces_error_get("chdir",
						t_strconcat(home, "/", NULL)));
			} if (errno != ENOENT)
				i_error("chdir(%s) failed: %m", home);
			else if (mail_set->mail_debug)
				i_debug("Home dir not found: %s", home);
		}
	}

	if (mail_user_init(mail_user, error_r) < 0) {
		mail_user_unref(&mail_user);
		return -1;
	}
	if ((user->flags & MAIL_STORAGE_SERVICE_FLAG_NO_NAMESPACES) == 0) {
		if (mail_namespaces_init(mail_user, error_r) < 0) {
			mail_user_unref(&mail_user);
			return -1;
		}
	}
	*mail_user_r = mail_user;
	return 0;
}

void mail_storage_service_io_activate_user(struct mail_storage_service_user *user)
{
	i_set_failure_prefix("%s", user->log_prefix);
}

void mail_storage_service_io_deactivate_user(struct mail_storage_service_user *user)
{
	i_set_failure_prefix("%s", user->service_ctx->default_log_prefix);
}

void mail_storage_service_io_deactivate(struct mail_storage_service_ctx *ctx)
{
	i_set_failure_prefix("%s", ctx->default_log_prefix);
}

static void
mail_storage_service_init_log(struct mail_storage_service_ctx *ctx,
			      struct mail_storage_service_user *user,
			      struct mail_storage_service_privileges *priv)
{
	ctx->log_initialized = TRUE;
	T_BEGIN {
		string_t *str;

		str = t_str_new(256);
		var_expand(str, user->user_set->mail_log_prefix,
			   get_var_expand_table(ctx->service, user, &user->input, priv));
		user->log_prefix = p_strdup(user->pool, str_c(str));
	} T_END;

	master_service_init_log(ctx->service, user->log_prefix);

	if (master_service_get_client_limit(master_service) == 1)
		i_set_failure_send_prefix(user->log_prefix);
	user->ioloop_ctx = io_loop_context_new(current_ioloop);
	io_loop_context_add_callbacks(user->ioloop_ctx,
				      mail_storage_service_io_activate_user,
				      mail_storage_service_io_deactivate_user,
				      user);
}

static void mail_storage_service_time_moved(time_t old_time, time_t new_time)
{
	long diff = new_time - old_time;

	if (diff > 0) {
		if (diff > MAX_NOWARN_FORWARD_SECS)
			i_warning("Time jumped forwards %ld seconds", diff);
		return;
	}
	diff = -diff;

	if (diff > MAX_TIME_BACKWARDS_SLEEP) {
		i_fatal("Time just moved backwards by %ld seconds. "
			"This might cause a lot of problems, "
			"so I'll just kill myself now. "
			"http://wiki2.dovecot.org/TimeMovedBackwards", diff);
	} else {
		i_error("Time just moved backwards by %ld seconds. "
			"I'll sleep now until we're back in present. "
			"http://wiki2.dovecot.org/TimeMovedBackwards", diff);
		/* Sleep extra second to make sure usecs also grows. */
		diff++;

		while (diff > 0 && sleep(diff) != 0) {
			/* don't use sleep()'s return value, because
			   it could get us to a long loop in case
			   interrupts just keep coming */
			diff = old_time - time(NULL) + 1;
		}
	}
}

struct mail_storage_service_ctx *
mail_storage_service_init(struct master_service *service,
			  const struct setting_parser_info *set_roots[],
			  enum mail_storage_service_flags flags)
{
	struct mail_storage_service_ctx *ctx;
	const char *version;
	pool_t pool;
	unsigned int count;

	version = master_service_get_version_string(service);
	if (version != NULL && strcmp(version, PACKAGE_VERSION) != 0) {
		i_fatal("Version mismatch: libdovecot-storage.so is '%s', "
			"while the running Dovecot binary is '%s'",
			PACKAGE_VERSION, version);
	}

	if ((flags & MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP) != 0 &&
	    geteuid() != 0) {
		/* service { user } isn't root. the permission drop can't be
		   temporary. */
		flags &= ~MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP;
	}

	(void)umask(0077);
	io_loop_set_time_moved_callback(current_ioloop,
					mail_storage_service_time_moved);

        mail_storage_init();
	mail_storage_register_all();
	mailbox_list_register_all();

	pool = pool_alloconly_create("mail storage service", 2048);
	ctx = p_new(pool, struct mail_storage_service_ctx, 1);
	ctx->pool = pool;
	ctx->service = service;
	ctx->flags = flags;

	/* @UNSAFE */
	if (set_roots == NULL)
		count = 0;
	else
		for (count = 0; set_roots[count] != NULL; count++) ;
	ctx->set_roots =
		p_new(pool, const struct setting_parser_info *, count + 2);
	ctx->set_roots[0] = &mail_user_setting_parser_info;
	if (set_roots != NULL) {
		memcpy(ctx->set_roots + 1, set_roots,
		       sizeof(*ctx->set_roots) * count);
	}

	/* do all the global initialization. delay initializing plugins until
	   we drop privileges the first time. */
	if ((flags & MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT) == 0) {
		/* note: we may not have read any settings yet, so this logging
		   may still be going to wrong location */
		ctx->default_log_prefix =
			p_strconcat(pool, service->name, ": ", NULL);
		master_service_init_log(service, ctx->default_log_prefix);
	}
	dict_drivers_register_builtin();
	return ctx;
}

struct auth_master_connection *
mail_storage_service_get_auth_conn(struct mail_storage_service_ctx *ctx)
{
	i_assert(ctx->conn != NULL);
	return ctx->conn;
}

static enum mail_storage_service_flags
mail_storage_service_input_get_flags(struct mail_storage_service_ctx *ctx,
				     const struct mail_storage_service_input *input)
{
	enum mail_storage_service_flags flags;

	flags = (ctx->flags & ~input->flags_override_remove) |
		input->flags_override_add;
	if (input->no_userdb_lookup) {
		/* FIXME: for API backwards compatibility only */
		flags &= ~MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
	}
	return flags;
}

int mail_storage_service_read_settings(struct mail_storage_service_ctx *ctx,
				       const struct mail_storage_service_input *input,
				       pool_t pool,
				       const struct setting_parser_info **user_info_r,
				       const struct setting_parser_context **parser_r,
				       const char **error_r)
{
	struct master_service_settings_input set_input;
	const struct setting_parser_info *const *roots;
	struct master_service_settings_output set_output;
	const struct dynamic_settings_parser *dyn_parsers;
	enum mail_storage_service_flags flags;
	unsigned int i;

	ctx->config_permission_denied = FALSE;

	flags = input == NULL ? ctx->flags :
		mail_storage_service_input_get_flags(ctx, input);

	memset(&set_input, 0, sizeof(set_input));
	set_input.roots = ctx->set_roots;
	set_input.preserve_user = TRUE;
	/* settings reader may exec doveconf, which is going to clear
	   environment, and if we're not doing a userdb lookup we want to
	   use $HOME */
	set_input.preserve_home =
		(flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) == 0;
	set_input.use_sysexits =
		(flags & MAIL_STORAGE_SERVICE_FLAG_USE_SYSEXITS) != 0;

	if (input != NULL) {
		set_input.module = input->module;
		set_input.service = input->service;
		set_input.username = input->username;
		set_input.local_ip = input->local_ip;
		set_input.remote_ip = input->remote_ip;
	}
	if (input == NULL) {
		/* global settings read - don't create a cache for thi */
	} else if (ctx->set_cache == NULL) {
		ctx->set_cache_module = p_strdup(ctx->pool, set_input.module);
		ctx->set_cache_service = p_strdup(ctx->pool, set_input.service);
		ctx->set_cache = master_service_settings_cache_init(
			ctx->service, set_input.module, set_input.service);
	} else {
		/* already looked up settings at least once.
		   we really shouldn't be execing anymore. */
		set_input.never_exec = TRUE;
	}

	dyn_parsers = mail_storage_get_dynamic_parsers(pool);
	if (null_strcmp(set_input.module, ctx->set_cache_module) == 0 &&
	    null_strcmp(set_input.service, ctx->set_cache_service) == 0 &&
	    ctx->set_cache != NULL) {
		if (master_service_settings_cache_read(ctx->set_cache,
						       &set_input, dyn_parsers,
						       parser_r, error_r) < 0) {
			*error_r = t_strdup_printf(
				"Error reading configuration: %s", *error_r);
			return -1;
		}
	} else {
		settings_parser_dyn_update(pool, &set_input.roots, dyn_parsers);
		if (master_service_settings_read(ctx->service, &set_input,
						 &set_output, error_r) < 0) {
			*error_r = t_strdup_printf(
				"Error reading configuration: %s", *error_r);
			ctx->config_permission_denied =
				set_output.permission_denied;
			return -1;
		}
		*parser_r = ctx->service->set_parser;
	}

	roots = settings_parser_get_roots(*parser_r);
	for (i = 0; roots[i] != NULL; i++) {
		if (strcmp(roots[i]->module_name,
			   mail_user_setting_parser_info.module_name) == 0) {
			*user_info_r = roots[i];
			return 0;
		}
	}
	i_unreached();
	return -1;
}

void mail_storage_service_set_auth_conn(struct mail_storage_service_ctx *ctx,
					struct auth_master_connection *conn)
{
	i_assert(ctx->conn == NULL);
	i_assert(mail_user_auth_master_conn == NULL);

	ctx->conn = conn;
	mail_user_auth_master_conn = conn;
}

static void
mail_storage_service_first_init(struct mail_storage_service_ctx *ctx,
				const struct setting_parser_info *user_info,
				const struct mail_user_settings *user_set)
{
	enum auth_master_flags flags = 0;

	ctx->debug = mail_user_set_get_mail_debug(user_info, user_set);
	if (ctx->debug)
		flags |= AUTH_MASTER_FLAG_DEBUG;
	if ((ctx->flags & MAIL_STORAGE_SERVICE_FLAG_NO_IDLE_TIMEOUT) != 0)
		flags |= AUTH_MASTER_FLAG_NO_IDLE_TIMEOUT;
	mail_storage_service_set_auth_conn(ctx,
		auth_master_init(user_set->auth_socket_path, flags));
}

static void
mail_storage_service_load_modules(struct mail_storage_service_ctx *ctx,
				  const struct setting_parser_info *user_info,
				  const struct mail_user_settings *user_set)
{
	struct module_dir_load_settings mod_set;

	if (*user_set->mail_plugins == '\0')
		return;
	if ((ctx->flags & MAIL_STORAGE_SERVICE_FLAG_NO_PLUGINS) != 0)
		return;

	memset(&mod_set, 0, sizeof(mod_set));
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.binary_name = master_service_get_name(ctx->service);
	mod_set.setting_name = "mail_plugins";
	mod_set.require_init_funcs = TRUE;
	mod_set.debug = mail_user_set_get_mail_debug(user_info, user_set);

	mail_storage_service_modules =
		module_dir_load_missing(mail_storage_service_modules,
					user_set->mail_plugin_dir,
					user_set->mail_plugins, &mod_set);
}

static int extra_field_key_cmp_p(const char *const *s1, const char *const *s2)
{
	const char *p1 = *s1, *p2 = *s2;

	for (; *p1 == *p2; p1++, p2++) {
		if (*p1 == '\0')
			return 0;
	}
	if (*p1 == '=')
		return -1;
	if (*p2 == '=')
		return 1;
	return *p1 - *p2;
}

int mail_storage_service_lookup(struct mail_storage_service_ctx *ctx,
				const struct mail_storage_service_input *input,
				struct mail_storage_service_user **user_r,
				const char **error_r)
{
	enum mail_storage_service_flags flags;
	struct mail_storage_service_user *user;
	const char *username = input->username;
	const struct setting_parser_info *user_info;
	const struct mail_user_settings *user_set;
	const char *const *userdb_fields, *error;
	struct auth_user_reply reply;
	const struct setting_parser_context *set_parser;
	void **sets;
	pool_t user_pool, temp_pool;
	int ret = 1;

	user_pool = pool_alloconly_create(MEMPOOL_GROWING"mail storage service user", 1024*6);
	flags = mail_storage_service_input_get_flags(ctx, input);

	if ((flags & MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP) != 0 &&
	    geteuid() != 0) {
		/* we dropped privileges only temporarily. switch back to root
		   before reading settings, so we'll definitely have enough
		   permissions to connect to the config socket. */
		mail_storage_service_seteuid_root();
	}

	if (mail_storage_service_read_settings(ctx, input, user_pool,
					       &user_info, &set_parser,
					       &error) < 0) {
		if (ctx->config_permission_denied) {
			/* just restart and maybe next time we will open the
			   config socket before dropping privileges */
			i_fatal("user %s: %s", username, error);
		}
		i_error("user %s: %s", username, error);
		pool_unref(&user_pool);
		*error_r = MAIL_ERRSTR_CRITICAL_MSG;
		return -1;
	}

	if ((flags & MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT) == 0 &&
	    !ctx->log_initialized) {
		/* initialize logging again, in case we only read the
		   settings for the first above */
		ctx->log_initialized = TRUE;
		master_service_init_log(ctx->service,
			t_strconcat(ctx->service->name, ": ", NULL));
	}
	sets = master_service_settings_parser_get_others(master_service,
							 set_parser);
	user_set = sets[0];

	if (ctx->conn == NULL)
		mail_storage_service_first_init(ctx, user_info, user_set);
	/* load global plugins */
	mail_storage_service_load_modules(ctx, user_info, user_set);

	if (ctx->userdb_next_pool == NULL)
		temp_pool = pool_alloconly_create("userdb lookup", 2048);
	else {
		temp_pool = ctx->userdb_next_pool;
		ctx->userdb_next_pool = NULL;
		pool_ref(temp_pool);
	}
	if ((flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) != 0) {
		ret = service_auth_userdb_lookup(ctx, input, temp_pool,
						 &username, &userdb_fields,
						 error_r);
		if (ret <= 0) {
			pool_unref(&temp_pool);
			pool_unref(&user_pool);
			return ret;
		}
		if (ctx->userdb_next_fieldsp != NULL)
			*ctx->userdb_next_fieldsp = userdb_fields;
	} else {
		userdb_fields = input->userdb_fields;
	}

	user = p_new(user_pool, struct mail_storage_service_user, 1);
	user->service_ctx = ctx;
	user->pool = user_pool;
	user->input = *input;
	user->input.userdb_fields = NULL;
	user->input.username = p_strdup(user_pool, username);
	user->input.session_id = p_strdup(user_pool, input->session_id);
	user->user_info = user_info;
	user->flags = flags;

	user->set_parser = settings_parser_dup(set_parser, user_pool);
	if (!settings_parser_check(user->set_parser, user_pool, &error))
		i_panic("settings_parser_check() failed: %s", error);

	sets = master_service_settings_parser_get_others(master_service,
							 user->set_parser);
	user->user_set = sets[0];
	user->gid_source = "mail_gid setting";
	user->uid_source = "mail_uid setting";

	if ((flags & MAIL_STORAGE_SERVICE_FLAG_DEBUG) != 0)
		(void)settings_parse_line(user->set_parser, "mail_debug=yes");

	if ((flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) == 0) {
		const char *home = getenv("HOME");
		if (home != NULL)
			set_keyval(ctx, user, "mail_home", home);
	}

	if (userdb_fields != NULL) {
		auth_user_fields_parse(userdb_fields, temp_pool, &reply);
		array_sort(&reply.extra_fields, extra_field_key_cmp_p);
		if (user_reply_handle(ctx, user, &reply, &error) < 0) {
			i_error("user %s: Invalid settings in userdb: %s",
				username, error);
			*error_r = ERRSTR_INVALID_USER_SETTINGS;
			ret = -2;
		}
	}
	pool_unref(&temp_pool);

	/* load per-user plugins */
	if (ret > 0) {
		mail_storage_service_load_modules(ctx, user_info,
						  user->user_set);
	}

	*user_r = user;
	return ret;
}

void mail_storage_service_save_userdb_fields(struct mail_storage_service_ctx *ctx,
					     pool_t pool, const char *const **userdb_fields_r)
{
	i_assert(pool != NULL);
	i_assert(userdb_fields_r != NULL);

	ctx->userdb_next_pool = pool;
	ctx->userdb_next_fieldsp = userdb_fields_r;
	*userdb_fields_r = NULL;
}

int mail_storage_service_next(struct mail_storage_service_ctx *ctx,
			      struct mail_storage_service_user *user,
			      struct mail_user **mail_user_r)
{
	struct mail_storage_service_privileges priv;
	const char *error;
	unsigned int len;
	bool disallow_root =
		(user->flags & MAIL_STORAGE_SERVICE_FLAG_DISALLOW_ROOT) != 0;
	bool temp_priv_drop =
		(user->flags & MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP) != 0;
	bool use_chroot;

	if (service_parse_privileges(ctx, user, &priv, &error) < 0) {
		i_error("user %s: %s", user->input.username, error);
		return -2;
	}

	if (*priv.home != '/' && *priv.home != '\0') {
		i_error("user %s: "
			"Relative home directory paths not supported: %s",
			user->input.username, priv.home);
		return -2;
	}

	/* we can't chroot if we want to switch between users. there's
	   not much point either (from security point of view). but if we're
	   already chrooted, we'll just have to continue and hope that the
	   current chroot is the same as the wanted chroot */
	use_chroot = !temp_priv_drop ||
		restrict_access_get_current_chroot() != NULL;

	len = strlen(priv.chroot);
	if (len > 2 && strcmp(priv.chroot + len - 2, "/.") == 0 &&
	    strncmp(priv.home, priv.chroot, len - 2) == 0) {
		/* mail_chroot = /chroot/. means that the home dir already
		   contains the chroot dir. remove it from home. */
		if (use_chroot) {
			priv.home += len - 2;
			if (*priv.home == '\0')
				priv.home = "/";
			priv.chroot = t_strndup(priv.chroot, len - 2);

			set_keyval(ctx, user, "mail_home", priv.home);
			set_keyval(ctx, user, "mail_chroot", priv.chroot);
		}
	} else if (len > 0 && !use_chroot) {
		/* we're not going to chroot. fix home directory so we can
		   access it. */
		if (*priv.home == '\0' || strcmp(priv.home, "/") == 0)
			priv.home = priv.chroot;
		else
			priv.home = t_strconcat(priv.chroot, priv.home, NULL);
		priv.chroot = "";
		set_keyval(ctx, user, "mail_home", priv.home);
	}

	if ((user->flags & MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT) == 0)
		mail_storage_service_init_log(ctx, user, &priv);

	if ((user->flags & MAIL_STORAGE_SERVICE_FLAG_NO_RESTRICT_ACCESS) == 0) {
		if (service_drop_privileges(user, &priv,
					    disallow_root, temp_priv_drop,
					    FALSE, &error) < 0) {
			i_error("user %s: Couldn't drop privileges: %s",
				user->input.username, error);
			return -1;
		}
		if (!temp_priv_drop ||
		    (user->flags & MAIL_STORAGE_SERVICE_FLAG_ENABLE_CORE_DUMPS) != 0)
			restrict_access_allow_coredumps(TRUE);
	}

	/* privileges are dropped. initialize plugins that haven't been
	   initialized yet. */
	module_dir_init(mail_storage_service_modules);

	if (mail_storage_service_init_post(ctx, user, &priv,
					   mail_user_r, &error) < 0) {
		i_error("user %s: Initialization failed: %s",
			user->input.username, error);
		return -2;
	}
	return 0;
}

void mail_storage_service_restrict_setenv(struct mail_storage_service_ctx *ctx,
					  struct mail_storage_service_user *user)
{
	struct mail_storage_service_privileges priv;
	const char *error;

	if (service_parse_privileges(ctx, user, &priv, &error) < 0)
		i_fatal("user %s: %s", user->input.username, error);
	if (service_drop_privileges(user, &priv,
				    FALSE, FALSE, TRUE, &error) < 0)
		i_fatal("user %s: %s", user->input.username, error);
}

int mail_storage_service_lookup_next(struct mail_storage_service_ctx *ctx,
				     const struct mail_storage_service_input *input,
				     struct mail_storage_service_user **user_r,
				     struct mail_user **mail_user_r,
				     const char **error_r)
{
	struct mail_storage_service_user *user;
	int ret;

	ret = mail_storage_service_lookup(ctx, input, &user, error_r);
	if (ret <= 0)
		return ret;

	ret = mail_storage_service_next(ctx, user, mail_user_r);
	if (ret < 0) {
		mail_storage_service_user_free(&user);
		*error_r = ret == -2 ? ERRSTR_INVALID_USER_SETTINGS :
			MAIL_ERRSTR_CRITICAL_MSG;
		return ret;
	}
	*user_r = user;
	return 1;
}

void mail_storage_service_user_free(struct mail_storage_service_user **_user)
{
	struct mail_storage_service_user *user = *_user;

	*_user = NULL;

	if (user->ioloop_ctx != NULL) {
		io_loop_context_remove_callbacks(user->ioloop_ctx,
			mail_storage_service_io_activate_user,
			mail_storage_service_io_deactivate_user, user);
		io_loop_context_unref(&user->ioloop_ctx);
	}
	settings_parser_deinit(&user->set_parser);
	pool_unref(&user->pool);
}

void mail_storage_service_init_settings(struct mail_storage_service_ctx *ctx,
					const struct mail_storage_service_input *input)
{
	const struct setting_parser_info *user_info;
	const struct mail_user_settings *user_set;
	const struct setting_parser_context *set_parser;
	const char *error;
	pool_t temp_pool;
	void **sets;

	if (ctx->conn != NULL)
		return;

	temp_pool = pool_alloconly_create("service all settings", 4096);
	if (mail_storage_service_read_settings(ctx, input, temp_pool,
					       &user_info, &set_parser,
					       &error) < 0)
		i_fatal("%s", error);
	sets = master_service_settings_parser_get_others(master_service,
							 set_parser);
	user_set = sets[0];

	mail_storage_service_first_init(ctx, user_info, user_set);
	pool_unref(&temp_pool);
}

static int
mail_storage_service_all_iter_deinit(struct mail_storage_service_ctx *ctx)
{
	int ret = 0;

	if (ctx->auth_list != NULL) {
		ret = auth_master_user_list_deinit(&ctx->auth_list);
		auth_master_deinit(&ctx->iter_conn);
	}
	return ret;
}

void mail_storage_service_all_init(struct mail_storage_service_ctx *ctx)
{
	enum auth_master_flags flags = 0;

	(void)mail_storage_service_all_iter_deinit(ctx);
	mail_storage_service_init_settings(ctx, NULL);

	/* create a new connection, because the iteration might take a while
	   and we might want to do USER lookups during it, which don't mix
	   well in the same connection. */
	if (ctx->debug)
		flags |= AUTH_MASTER_FLAG_DEBUG;
	ctx->iter_conn = auth_master_init(auth_master_get_socket_path(ctx->conn),
					  flags);
	ctx->auth_list = auth_master_user_list_init(ctx->iter_conn, "", NULL);
}

int mail_storage_service_all_next(struct mail_storage_service_ctx *ctx,
				  const char **username_r)
{
	i_assert((ctx->flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) != 0);

	*username_r = auth_master_user_list_next(ctx->auth_list);
	if (*username_r != NULL)
		return 1;
	return mail_storage_service_all_iter_deinit(ctx);
}

void mail_storage_service_deinit(struct mail_storage_service_ctx **_ctx)
{
	struct mail_storage_service_ctx *ctx = *_ctx;

	*_ctx = NULL;
	(void)mail_storage_service_all_iter_deinit(ctx);
	if (ctx->conn != NULL) {
		if (mail_user_auth_master_conn == ctx->conn)
			mail_user_auth_master_conn = NULL;
		auth_master_deinit(&ctx->conn);
	}
	if (ctx->set_cache != NULL)
		master_service_settings_cache_deinit(&ctx->set_cache);
	pool_unref(&ctx->pool);

	module_dir_unload(&mail_storage_service_modules);
	mail_storage_deinit();
	dict_drivers_unregister_builtin();
}

void **mail_storage_service_user_get_set(struct mail_storage_service_user *user)
{
	return master_service_settings_parser_get_others(master_service,
							 user->set_parser);
}

const struct mail_storage_settings *
mail_storage_service_user_get_mail_set(struct mail_storage_service_user *user)
{
	return mail_user_set_get_driver_settings(
				user->user_info, user->user_set,
				MAIL_STORAGE_SET_DRIVER_NAME);
}

const struct mail_storage_service_input *
mail_storage_service_user_get_input(struct mail_storage_service_user *user)
{
	return &user->input;
}

struct setting_parser_context *
mail_storage_service_user_get_settings_parser(struct mail_storage_service_user *user)
{
	return user->set_parser;
}

void *mail_storage_service_get_settings(struct master_service *service)
{
	void **sets, *set;

	T_BEGIN {
		sets = master_service_settings_get_others(service);
		set = sets[1];
	} T_END;
	return set;
}
