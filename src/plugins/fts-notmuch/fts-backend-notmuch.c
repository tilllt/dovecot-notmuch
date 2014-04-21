#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "strescape.h"
#include "unichar.h"
#include "mail-storage-private.h"
#include "mailbox-list-private.h"
#include "mail-search.h"
#include "fts-api.h"
#include "fts-notmuch-plugin.h"

#include <ctype.h>
#include <syslog.h>
#include <unistd.h>

#include <notmuch.h>

struct notmuch_fts_backend {
	struct fts_backend backend;
};

static struct fts_backend *fts_backend_notmuch_alloc(void)
{
	struct notmuch_fts_backend *backend;

	backend = i_new(struct notmuch_fts_backend, 1);
	backend->backend = fts_backend_notmuch;
	return &backend->backend;
}

static int
fts_backend_notmuch_init(struct fts_backend *_backend,
                         const char **error_r ATTR_UNUSED)
{
	return 0;
}

static void
fts_backend_notmuch_deinit(struct fts_backend *_backend)
{
	struct notmuch_fts_backend *backend = (struct notmuch_fts_backend *)_backend;

	i_free(backend);
}

static int
fts_backend_notmuch_get_last_uid(struct fts_backend *_backend,
                                 struct mailbox *box, uint32_t *last_uid_r)
{
	struct fts_index_header hdr;

	if (fts_index_get_header(box, &hdr)) {
		*last_uid_r = hdr.last_indexed_uid;
		return 0;
	}

	*last_uid_r = 0;
	(void)fts_index_set_last_uid(box, *last_uid_r);
	return 0;
}

static struct fts_backend_update_context *
fts_backend_notmuch_update_init(struct fts_backend *backend)
{
	struct fts_backend_update_context *ctx;

	ctx = i_new(struct fts_backend_update_context, 1);
	ctx->backend = backend;
	return ctx;
}

static int
fts_backend_notmuch_update_deinit(struct fts_backend_update_context *ctx)
{
	i_free(ctx);
	return 0;
}

static void
fts_backend_notmuch_update_set_mailbox(struct fts_backend_update_context *ctx,
                                       struct mailbox *box)
{
	return;
}

static void
fts_backend_notmuch_update_expunge(struct fts_backend_update_context *ctx, uint32_t uid)
{
	return;
}

static bool
fts_backend_notmuch_update_set_build_key(struct fts_backend_update_context *ctx,
                                         const struct fts_backend_build_key *key)
{
	return TRUE;
}

static void
fts_backend_notmuch_update_unset_build_key(struct fts_backend_update_context *ctx)
{
	return;
}

static int
fts_backend_notmuch_update_build_more(struct fts_backend_update_context *ctx,
                                      const unsigned char *data, size_t size)
{
	return 0;
}

static int fts_backend_notmuch_refresh(struct fts_backend *backend ATTR_UNUSED)
{
	return 0;
}

static int fts_backend_notmuch_rescan(struct fts_backend *backend)
{
	return 0;
}

static int fts_backend_notmuch_optimize(struct fts_backend *backend ATTR_UNUSED)
{
	return 0;
}

static int
notmuch_search(const struct mailbox *box, const char *terms,
               ARRAY_TYPE(seq_range) *uids)
{
	notmuch_database_t *notmuch;
	notmuch_query_t *query;
	notmuch_message_t *message;
	notmuch_messages_t *messages;
	notmuch_filenames_t *filenames;
	uint32_t uid;

/*
	if (notmuch_database_open(notmuch_config_get_database_path("/home/jwm/.maildir"),
	                          NOTMUCH_DATABASE_MODE_READ_ONLY, &notmuch))
		return -1;
*/
	if (notmuch_database_open("/home/jwm/.maildir",
	                          NOTMUCH_DATABASE_MODE_READ_ONLY, &notmuch))
		return -1;

	query = notmuch_query_create(notmuch, terms);
	if (query == NULL) {
		return -1;
	}

	messages = notmuch_query_search_messages(query);
	if (messages == NULL) {
		return -1;
	}

	for (;
	     notmuch_messages_valid(messages);
	     notmuch_messages_move_to_next(messages))
	{
		message = notmuch_messages_get(messages);

		filenames = notmuch_message_get_filenames(message);
		for (;
		     notmuch_filenames_valid(filenames);
		     notmuch_filenames_move_to_next(filenames))
		{
/*
			maildir_uidlist_get_uid(box->uidlist,
				notmuch_filenames_get(filenames), &uid);
*/
			uid = 69;
			syslog(LOG_INFO, "got uid %u", uid);
			seq_range_array_add(uids, 1, uid);
		}
		notmuch_filenames_destroy(filenames);

		notmuch_message_destroy(message);
	}

	notmuch_messages_destroy(messages);
	notmuch_query_destroy(query);
	notmuch_database_destroy(notmuch);
	return 0;
}

static int
fts_backend_notmuch_lookup(struct fts_backend *_backend, struct mailbox *box,
			struct mail_search_arg *args, bool and_args,
			struct fts_result *result)
{
	struct mailbox_status status;
	string_t *str;
	const char *box_guid;
	unsigned int prefix_len;
	uint32_t item;

	if (fts_mailbox_get_guid(box, &box_guid) < 0)
		return -1;
	mailbox_get_open_status(box, STATUS_UIDNEXT, &status);

	openlog("dovecot-notmuch", LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "adding uid 42");
/*	sleep(15);*/
    seq_range_array_add(&result->definite_uids, 1, 42);
/*    item = 42;
    array_append_i(&result->definite_uids, &item, 1);*/
    item = 500;
/*    array_append_i(&result->scores, &item, 1);*/
/*    result->scores_sorted = TRUE;*/
    args->match_always = TRUE;
/*
	if (notmuch_search(box, "FIXME", &result->definite_uids) < 0)
		return -1;
*/
	return 0;
}

static int
notmuch_search_multi(struct fts_backend *_backend, string_t *str,
		  struct mailbox *const boxes[],
		  struct fts_multi_result *result)
{
/*
	p_array_init(&fts_results, result->pool, 32);
	for (i = 0; notmuch_results[i] != NULL; i++) {
		fts_result = array_append_space(&fts_results);
		fts_result->box = box;
		fts_result->definite_uids = notmuch_results[i]->uids;
	}
	(void)array_append_space(&fts_results);
	result->box_results = array_idx_modifiable(&fts_results, 0);
	hash_table_destroy(&mailboxes);
*/
	return 0;
}

static int
fts_backend_notmuch_lookup_multi(struct fts_backend *backend,
                  struct mailbox *const boxes[],
                  struct mail_search_arg *args, bool and_args,
                  struct fts_multi_result *result)
{
	/* FIXME */
	return 0;
}

struct fts_backend fts_backend_notmuch = {
	.name = "notmuch",
	.flags = FTS_BACKEND_FLAG_FUZZY_SEARCH,

	{
		fts_backend_notmuch_alloc,
		fts_backend_notmuch_init,
		fts_backend_notmuch_deinit,
		fts_backend_notmuch_get_last_uid,
		fts_backend_notmuch_update_init,
		fts_backend_notmuch_update_deinit,
		fts_backend_notmuch_update_set_mailbox,
		fts_backend_notmuch_update_expunge,
		fts_backend_notmuch_update_set_build_key,
		fts_backend_notmuch_update_unset_build_key,
		fts_backend_notmuch_update_build_more,
		fts_backend_notmuch_refresh,
		fts_backend_notmuch_rescan,
		fts_backend_notmuch_optimize,
		fts_backend_default_can_lookup,
		fts_backend_notmuch_lookup,
		fts_backend_notmuch_lookup_multi,
		NULL
	}
};
