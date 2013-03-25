#ifndef DOVEADM_MAIL_H
#define DOVEADM_MAIL_H

#include <stdio.h>
#include "doveadm.h"
#include "doveadm-util.h"
#include "module-context.h"
#include "mail-storage-service.h"

enum mail_error;
struct mailbox;
struct mail_storage;
struct mail_user;
struct doveadm_mail_cmd_context;

struct doveadm_mail_cmd_vfuncs {
	bool (*parse_arg)(struct doveadm_mail_cmd_context *ctx,int c);
	void (*preinit)(struct doveadm_mail_cmd_context *ctx);
	void (*init)(struct doveadm_mail_cmd_context *ctx,
		     const char *const args[]);
	int (*get_next_user)(struct doveadm_mail_cmd_context *ctx,
			     const char **username_r);
	int (*prerun)(struct doveadm_mail_cmd_context *ctx,
		      struct mail_storage_service_user *service_user,
		      const char **error_r);
	int (*run)(struct doveadm_mail_cmd_context *ctx,
		   struct mail_user *mail_user);
	void (*deinit)(struct doveadm_mail_cmd_context *ctx);
};

struct doveadm_mail_cmd_module_register {
	unsigned int id;
};

union doveadm_mail_cmd_module_context {
        struct doveadm_mail_cmd_vfuncs super;
	struct doveadm_mail_cmd_module_register *reg;
};

struct doveadm_mail_cmd_context {
	pool_t pool;
	const struct doveadm_mail_cmd *cmd;
	const char *const *args;
	/* args including -options */
	const char *const *full_args;

	const char *getopt_args;
	const struct doveadm_settings *set;
	enum mail_storage_service_flags service_flags;
	struct mail_storage_service_ctx *storage_service;
	struct mail_storage_service_input storage_service_input;
	/* search args aren't set for all mail commands */
	struct mail_search_args *search_args;

	const char *cur_username;
	struct mail_storage_service_user *cur_service_user;
	struct mail_user *cur_mail_user;
	struct doveadm_mail_cmd_vfuncs v;

	ARRAY_DEFINE(module_contexts, union doveadm_mail_cmd_module_context *);

	/* if non-zero, exit with this code */
	int exit_code;

	/* This command is being called by a remote doveadm client. */
	unsigned int proxying:1;
	/* We're handling only a single user */
	unsigned int iterate_single_user:1;
	/* We're going through all users (not set for wildcard usernames) */
	unsigned int iterate_all_users:1;
};

struct doveadm_mail_cmd {
	struct doveadm_mail_cmd_context *(*alloc)(void);
	const char *name;
	const char *usage_args;
};
ARRAY_DEFINE_TYPE(doveadm_mail_cmd, struct doveadm_mail_cmd);

extern ARRAY_TYPE(doveadm_mail_cmd) doveadm_mail_cmds;
extern void (*hook_doveadm_mail_init)(struct doveadm_mail_cmd_context *ctx);
extern struct doveadm_mail_cmd_module_register doveadm_mail_cmd_module_register;
extern char doveadm_mail_cmd_hide;

bool doveadm_mail_try_run(const char *cmd_name, int argc, char *argv[]);
void doveadm_mail_register_cmd(const struct doveadm_mail_cmd *cmd);
const struct doveadm_mail_cmd *doveadm_mail_cmd_find(const char *cmd_name);

void doveadm_mail_usage(string_t *out);
void doveadm_mail_help(const struct doveadm_mail_cmd *cmd) ATTR_NORETURN;
void doveadm_mail_help_name(const char *cmd_name) ATTR_NORETURN;
void doveadm_mail_try_help_name(const char *cmd_name);
bool doveadm_mail_has_subcommands(const char *cmd_name);

void doveadm_mail_init(void);
void doveadm_mail_deinit(void);

const struct doveadm_mail_cmd *
doveadm_mail_cmd_find_from_argv(const char *cmd_name, int *argc,
				const char *const **argv);
struct doveadm_mail_cmd_context *
doveadm_mail_cmd_init(const struct doveadm_mail_cmd *cmd,
		      const struct doveadm_settings *set);
int doveadm_mail_single_user(struct doveadm_mail_cmd_context *ctx,
			     const struct mail_storage_service_input *input,
			     const char **error_r);
int doveadm_mail_server_user(struct doveadm_mail_cmd_context *ctx,
			     const struct mail_storage_service_input *input,
			     const char **error_r);
void doveadm_mail_server_flush(void);

struct mailbox *
doveadm_mailbox_find(struct mail_user *user, const char *mailbox);
int doveadm_mailbox_find_and_sync(struct mail_user *user, const char *mailbox,
				  struct mailbox **box_r);
struct mail_search_args *
doveadm_mail_build_search_args(const char *const args[]);
void doveadm_mailbox_args_check(const char *const args[]);
struct mail_search_args *
doveadm_mail_mailbox_search_args_build(const char *const args[]);

void expunge_search_args_check(struct mail_search_args *args, const char *cmd);

struct doveadm_mail_cmd_context *
doveadm_mail_cmd_alloc_size(size_t size);
#define doveadm_mail_cmd_alloc(type) \
	(type *)doveadm_mail_cmd_alloc_size(sizeof(type))

void doveadm_mail_failed_error(struct doveadm_mail_cmd_context *ctx,
			       enum mail_error error);
void doveadm_mail_failed_storage(struct doveadm_mail_cmd_context *ctx,
				 struct mail_storage *storage);
void doveadm_mail_failed_mailbox(struct doveadm_mail_cmd_context *ctx,
				 struct mailbox *box);

struct doveadm_mail_cmd cmd_expunge;
struct doveadm_mail_cmd cmd_search;
struct doveadm_mail_cmd cmd_fetch;
struct doveadm_mail_cmd cmd_import;
struct doveadm_mail_cmd cmd_index;
struct doveadm_mail_cmd cmd_altmove;
struct doveadm_mail_cmd cmd_copy;
struct doveadm_mail_cmd cmd_move;
struct doveadm_mail_cmd cmd_mailbox_list;
struct doveadm_mail_cmd cmd_mailbox_create;
struct doveadm_mail_cmd cmd_mailbox_delete;
struct doveadm_mail_cmd cmd_mailbox_rename;
struct doveadm_mail_cmd cmd_mailbox_subscribe;
struct doveadm_mail_cmd cmd_mailbox_unsubscribe;
struct doveadm_mail_cmd cmd_mailbox_status;
struct doveadm_mail_cmd cmd_batch;

#endif
