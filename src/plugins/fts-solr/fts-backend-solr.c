/* Copyright (c) 2006-2011 Dovecot authors, see the included COPYING file */

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
#include "solr-connection.h"
#include "fts-solr-plugin.h"

#include <ctype.h>

#define SOLR_CMDBUF_SIZE (1024*64)
#define SOLR_MAX_MULTI_ROWS 100000

struct solr_fts_backend {
	struct fts_backend backend;
};

struct solr_fts_backend_update_context {
	struct fts_backend_update_context ctx;

	struct mailbox *cur_box;
	char box_guid[MAILBOX_GUID_HEX_LENGTH+1];

	struct solr_connection_post *post;
	uint32_t prev_uid;
	string_t *cmd, *hdr, *hdr_fields;

	bool headers_open;
	bool cur_header_index;
	bool documents_added;
};

static struct solr_connection *solr_conn = NULL;

static bool is_valid_xml_char(unichar_t chr)
{
	/* Valid characters in XML:

	   #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] |
	   [#x10000-#x10FFFF]

	   This function gets called only for #x80 and higher */
	if (chr > 0xd7ff && chr < 0xe000)
		return FALSE;
	if (chr > 0xfffd && chr < 0x10000)
		return FALSE;
	return chr < 0x10ffff;
}

static void
xml_encode_data(string_t *dest, const unsigned char *data, unsigned int len)
{
	unichar_t chr;
	unsigned int i;

	for (i = 0; i < len; i++) {
		switch (data[i]) {
		case '&':
			str_append(dest, "&amp;");
			break;
		case '<':
			str_append(dest, "&lt;");
			break;
		case '>':
			str_append(dest, "&gt;");
			break;
		case '\t':
		case '\n':
		case '\r':
			/* exceptions to the following control char check */
			str_append_c(dest, data[i]);
			break;
		default:
			if (data[i] < 32) {
				/* SOLR doesn't like control characters.
				   replace them with spaces. */
				str_append_c(dest, ' ');
			} else if (data[i] >= 0x80) {
				/* make sure the character is valid for XML
				   so we don't get XML parser errors */
				unsigned int char_len =
					uni_utf8_char_bytes(data[i]);
				if (i + char_len <= len &&
				    uni_utf8_get_char_n(data + i, char_len, &chr) == 1 &&
				    is_valid_xml_char(chr))
					str_append_n(dest, data + i, char_len);
				else {
					str_append_n(dest, utf8_replacement_char,
						     UTF8_REPLACEMENT_CHAR_LEN);
				}
				i += char_len - 1;
			} else {
				str_append_c(dest, data[i]);
			}
			break;
		}
	}
}

static void xml_encode(string_t *dest, const char *str)
{
	xml_encode_data(dest, (const unsigned char *)str, strlen(str));
}

static void solr_quote_http(string_t *dest, const char *str)
{
	str_append(dest, "%22");
	solr_connection_http_escape(solr_conn, dest, str);
	str_append(dest, "%22");
}

static struct fts_backend *fts_backend_solr_alloc(void)
{
	struct solr_fts_backend *backend;

	backend = i_new(struct solr_fts_backend, 1);
	backend->backend = fts_backend_solr;
	return &backend->backend;
}

static int
fts_backend_solr_init(struct fts_backend *_backend,
		      const char **error_r ATTR_UNUSED)
{
	struct fts_solr_user *fuser = FTS_SOLR_USER_CONTEXT(_backend->ns->user);
	const struct fts_solr_settings *set = &fuser->set;

	if (solr_conn == NULL)
		solr_conn = solr_connection_init(set->url, set->debug);
	return 0;
}

static void fts_backend_solr_deinit(struct fts_backend *_backend)
{
	struct solr_fts_backend *backend = (struct solr_fts_backend *)_backend;

	i_free(backend);
}

static int
get_last_uid_fallback(struct fts_backend *_backend, struct mailbox *box,
		      uint32_t *last_uid_r)
{
	const struct seq_range *uidvals;
	const char *box_guid;
	unsigned int count;
	struct solr_result **results;
	string_t *str;
	pool_t pool;
	int ret = 0;

	str = t_str_new(256);
	str_append(str, "fl=uid&rows=1&sort=uid+desc&q=");

	if (fts_mailbox_get_guid(box, &box_guid) < 0)
		return -1;

	str_printfa(str, "box:%s+user:", box_guid);
	if (_backend->ns->owner != NULL)
		solr_quote_http(str, _backend->ns->owner->username);
	else
		str_append(str, "%22%22");

	pool = pool_alloconly_create("solr last uid lookup", 1024);
	if (solr_connection_select(solr_conn, str_c(str),
				   pool, &results) < 0)
		ret = -1;
	else if (results[0] == NULL) {
		/* no UIDs */
		*last_uid_r = 0;
	} else {
		uidvals = array_get(&results[0]->uids, &count);
		i_assert(count > 0);
		if (count == 1 && uidvals[0].seq1 == uidvals[0].seq2) {
			*last_uid_r = uidvals[0].seq1;
		} else {
			i_error("fts_solr: Last UID lookup returned multiple rows");
			ret = -1;
		}
	}
	pool_unref(&pool);
	return ret;
}

static int
fts_backend_solr_get_last_uid(struct fts_backend *_backend,
			      struct mailbox *box, uint32_t *last_uid_r)
{
	struct fts_index_header hdr;

	if (fts_index_get_header(box, &hdr)) {
		*last_uid_r = hdr.last_indexed_uid;
		return 0;
	}

	/* either nothing has been indexed, or the index was corrupted.
	   do it the slow way. */
	if (get_last_uid_fallback(_backend, box, last_uid_r) < 0)
		return -1;

	(void)fts_index_set_last_uid(box, *last_uid_r);
	return 0;
}

static struct fts_backend_update_context *
fts_backend_solr_update_init(struct fts_backend *_backend)
{
	struct solr_fts_backend_update_context *ctx;

	ctx = i_new(struct solr_fts_backend_update_context, 1);
	ctx->ctx.backend = _backend;
	ctx->cmd = str_new(default_pool, SOLR_CMDBUF_SIZE);
	ctx->hdr = str_new(default_pool, 4096);
	ctx->hdr_fields = str_new(default_pool, 1024);
	return &ctx->ctx;
}

static void xml_encode_id(struct solr_fts_backend_update_context *ctx,
			  string_t *str, uint32_t uid)
{
	str_printfa(str, "%u/%s", uid, ctx->box_guid);
	if (ctx->ctx.backend->ns->owner != NULL) {
		str_append_c(str, '/');
		xml_encode(str, ctx->ctx.backend->ns->owner->username);
	}
}

static void
fts_backend_solr_doc_open(struct solr_fts_backend_update_context *ctx,
			  uint32_t uid)
{
	ctx->documents_added = TRUE;

	str_printfa(ctx->cmd, "<doc>"
		    "<field name=\"uid\">%u</field>"
		    "<field name=\"box\">%s</field>",
		    uid, ctx->box_guid);
	str_append(ctx->cmd, "<field name=\"user\">");
	if (ctx->ctx.backend->ns->owner != NULL)
		xml_encode(ctx->cmd, ctx->ctx.backend->ns->owner->username);
	str_append(ctx->cmd, "</field>");

	str_printfa(ctx->cmd, "<field name=\"id\">");
	xml_encode_id(ctx, ctx->cmd, uid);
	str_append(ctx->cmd, "</field>");
}

static void
fts_backend_solr_doc_close(struct solr_fts_backend_update_context *ctx)
{
	ctx->headers_open = FALSE;
	if (str_len(ctx->hdr) > 0) {
		str_append(ctx->cmd, "<field name=\"hdr\">");
		str_append_str(ctx->cmd, ctx->hdr);
		str_append(ctx->cmd, "</field>");
		str_truncate(ctx->hdr, 0);
	}
	if (str_len(ctx->hdr_fields) > 0) {
		str_append_str(ctx->cmd, ctx->hdr_fields);
		str_truncate(ctx->hdr_fields, 0);
	}
	str_append(ctx->cmd, "</doc>");
}

static int
fts_backed_solr_build_commit(struct solr_fts_backend_update_context *ctx)
{
	if (ctx->post == NULL)
		return 0;

	fts_backend_solr_doc_close(ctx);
	str_append(ctx->cmd, "</add>");

	solr_connection_post_more(ctx->post, str_data(ctx->cmd),
				  str_len(ctx->cmd));
	return solr_connection_post_end(ctx->post);
}

static int
fts_backend_solr_update_deinit(struct fts_backend_update_context *_ctx)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;
	const char *str;
	int ret = _ctx->failed ? -1 : 0;

	if (fts_backed_solr_build_commit(ctx) < 0)
		ret = -1;

	/* commit and wait until the documents we just indexed are
	   visible to the following search */
	str = t_strdup_printf("<commit waitFlush=\"false\" "
			      "waitSearcher=\"%s\"/>",
			      ctx->documents_added ? "true" : "false");
	if (solr_connection_post(solr_conn, str) < 0)
		ret = -1;

	str_free(&ctx->cmd);
	str_free(&ctx->hdr);
	str_free(&ctx->hdr_fields);
	i_free(ctx);
	return ret;
}

static void
fts_backend_solr_update_set_mailbox(struct fts_backend_update_context *_ctx,
				    struct mailbox *box)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;
	const char *box_guid;

	ctx->cur_box = box;

	if (box != NULL) {
		if (fts_mailbox_get_guid(box, &box_guid) < 0)
			_ctx->failed = TRUE;

		i_assert(strlen(box_guid) == sizeof(ctx->box_guid)-1);
		memcpy(ctx->box_guid, box_guid, sizeof(ctx->box_guid)-1);
	} else {
		memset(ctx->box_guid, 0, sizeof(ctx->box_guid));
	}
}

static void
fts_backend_solr_update_expunge(struct fts_backend_update_context *_ctx,
				uint32_t uid)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;

	T_BEGIN {
		string_t *cmd;

		cmd = t_str_new(256);
		str_append(cmd, "<delete><id>");
		xml_encode_id(ctx, cmd, uid);
		str_append(cmd, "</id></delete>");

		(void)solr_connection_post(solr_conn, str_c(cmd));
	} T_END;
}

static void
fts_backend_solr_uid_changed(struct solr_fts_backend_update_context *ctx,
			     uint32_t uid)
{
	if (ctx->post == NULL) {
		i_assert(ctx->prev_uid == 0);

		ctx->post = solr_connection_post_begin(solr_conn);
		str_append(ctx->cmd, "<add>");
	} else {
		fts_backend_solr_doc_close(ctx);
	}
	ctx->prev_uid = uid;
	fts_backend_solr_doc_open(ctx, uid);
}

static bool
fts_backend_solr_update_set_build_key(struct fts_backend_update_context *_ctx,
				      const struct fts_backend_build_key *key)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;

	if (key->uid != ctx->prev_uid)
		fts_backend_solr_uid_changed(ctx, key->uid);

	switch (key->type) {
	case FTS_BACKEND_BUILD_KEY_HDR:
		if (fts_header_want_indexed(key->hdr_name)) {
			ctx->cur_header_index = TRUE;
			str_printfa(ctx->hdr_fields, "<field name=\"%s\">",
				    t_str_lcase(key->hdr_name));
		}
		/* fall through */
	case FTS_BACKEND_BUILD_KEY_MIME_HDR:
		xml_encode(ctx->hdr, key->hdr_name);
		str_append(ctx->hdr, ": ");
		ctx->headers_open = TRUE;
		break;
	case FTS_BACKEND_BUILD_KEY_BODY_PART:
		ctx->headers_open = FALSE;
		str_append(ctx->cmd, "<field name=\"body\">");
		break;
	case FTS_BACKEND_BUILD_KEY_BODY_PART_BINARY:
		i_unreached();
	}
	return TRUE;
}

static void
fts_backend_solr_update_unset_build_key(struct fts_backend_update_context *_ctx)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;

	if (!ctx->headers_open)
		str_append(ctx->cmd, "</field>");
	else {
		/* this is called individually for each header line.
		   headers are finished only when key changes to body */
		str_append_c(ctx->hdr, '\n');
	}

	if (ctx->cur_header_index) {
		str_append(ctx->hdr_fields, "</field>");
		ctx->cur_header_index = FALSE;
	}
}

static int
fts_backend_solr_update_build_more(struct fts_backend_update_context *_ctx,
				   const unsigned char *data, size_t size)
{
	struct solr_fts_backend_update_context *ctx =
		(struct solr_fts_backend_update_context *)_ctx;

	if (_ctx->failed)
		return -1;

	if (ctx->headers_open) {
		if (ctx->cur_header_index)
			xml_encode_data(ctx->hdr_fields, data, size);
		xml_encode_data(ctx->hdr, data, size);
	} else {
		i_assert(!ctx->cur_header_index);
		xml_encode_data(ctx->cmd, data, size);
	}

	if (str_len(ctx->cmd) > SOLR_CMDBUF_SIZE-128) {
		solr_connection_post_more(ctx->post, str_data(ctx->cmd),
					  str_len(ctx->cmd));
		str_truncate(ctx->cmd, 0);
	}
	return 0;
}

static int fts_backend_solr_refresh(struct fts_backend *backend ATTR_UNUSED)
{
	return 0;
}

static int fts_backend_solr_optimize(struct fts_backend *backend ATTR_UNUSED)
{
	return 0;
}

static bool solr_need_escaping(const char *str)
{
	const char *solr_escape_chars = "+-&|!(){}[]^\"~*?:\\ ";

	for (; *str != '\0'; str++) {
		if (strchr(solr_escape_chars, *str) != NULL)
			return TRUE;
	}
	return FALSE;
}

static void solr_add_str_arg(string_t *str, struct mail_search_arg *arg)
{
	/* currently we'll just disable fuzzy searching if there are any
	   parameters that need escaping. solr doesn't seem to give good
	   fuzzy results even if we did escape them.. */
	if (!arg->fuzzy || solr_need_escaping(arg->value.str))
		solr_quote_http(str, arg->value.str);
	else {
		str_append(str, arg->value.str);
		str_append_c(str, '~');
	}
}

static bool
solr_add_definite_query(string_t *str, struct mail_search_arg *arg)
{
	switch (arg->type) {
	case SEARCH_TEXT: {
		if (arg->match_not)
			str_append_c(str, '-');
		str_append(str, "(hdr:");
		solr_add_str_arg(str, arg);
		str_append(str, "+OR+body:");
		solr_add_str_arg(str, arg);
		str_append(str, ")");
		break;
	}
	case SEARCH_BODY:
		if (arg->match_not)
			str_append_c(str, '-');
		str_append(str, "body:");
		solr_add_str_arg(str, arg);
		break;
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
	case SEARCH_HEADER_COMPRESS_LWSP:
		if (!fts_header_want_indexed(arg->hdr_field_name))
			return FALSE;

		if (arg->match_not)
			str_append_c(str, '-');
		str_append(str, arg->hdr_field_name);
		str_append_c(str, ':');
		solr_add_str_arg(str, arg);
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static bool
solr_add_definite_query_args(string_t *str, struct mail_search_arg *arg,
			     bool and_args)
{
	unsigned int last_len;

	last_len = str_len(str);
	for (; arg != NULL; arg = arg->next) {
		if (solr_add_definite_query(str, arg)) {
			arg->match_always = TRUE;
			last_len = str_len(str);
			if (and_args)
				str_append(str, "+AND+");
			else
				str_append(str, "+OR+");
		}
	}
	if (str_len(str) == last_len)
		return FALSE;

	str_truncate(str, last_len);
	return TRUE;
}

static bool
solr_add_maybe_query(string_t *str, struct mail_search_arg *arg)
{
	switch (arg->type) {
	case SEARCH_HEADER:
	case SEARCH_HEADER_ADDRESS:
	case SEARCH_HEADER_COMPRESS_LWSP:
		if (fts_header_want_indexed(arg->hdr_field_name))
			return FALSE;
		if (arg->match_not) {
			/* all matches would be definite, but all non-matches
			   would be maybies. too much trouble to optimize. */
			return FALSE;
		}

		/* we can check if the search key exists in some header and
		   filter out the messages that have no chance of matching */
		str_append(str, "hdr:");
		if (*arg->value.str != '\0')
			solr_quote_http(str, arg->value.str);
		else {
			/* checking potential existence of the header name */
			solr_quote_http(str, arg->hdr_field_name);
		}
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static bool
solr_add_maybe_query_args(string_t *str, struct mail_search_arg *arg,
			  bool and_args)
{
	unsigned int last_len;

	last_len = str_len(str);
	for (; arg != NULL; arg = arg->next) {
		if (solr_add_maybe_query(str, arg)) {
			arg->match_always = TRUE;
			last_len = str_len(str);
			if (and_args)
				str_append(str, "+AND+");
			else
				str_append(str, "+OR+");
		}
	}
	if (str_len(str) == last_len)
		return FALSE;

	str_truncate(str, last_len);
	return TRUE;
}

static int solr_search(struct fts_backend *_backend, string_t *str,
		       const char *box_guid, ARRAY_TYPE(seq_range) *uids_r,
		       ARRAY_TYPE(fts_score_map) *scores_r)
{
	pool_t pool = pool_alloconly_create("fts solr search", 1024);
	struct solr_result **results;
	int ret;

	/* use a separate filter query for selecting the mailbox. it shouldn't
	   affect the score and there could be some caching benefits too. */
	str_printfa(str, "&fq=%%2Bbox:%s+%%2Buser:", box_guid);
	if (_backend->ns->owner != NULL)
		solr_quote_http(str, _backend->ns->owner->username);
	else
		str_append(str, "%22%22");

	ret = solr_connection_select(solr_conn, str_c(str), pool, &results);
	if (ret == 0 && results[0] != NULL) {
		array_append_array(uids_r, &results[0]->uids);
		array_append_array(scores_r, &results[0]->scores);
	}
	pool_unref(&pool);
	return ret;
}

static int
fts_backend_solr_lookup(struct fts_backend *_backend, struct mailbox *box,
			struct mail_search_arg *args, bool and_args,
			struct fts_result *result)
{
	struct mailbox_status status;
	string_t *str;
	const char *box_guid;
	unsigned int prefix_len;

	if (fts_mailbox_get_guid(box, &box_guid) < 0)
		return -1;
	mailbox_get_open_status(box, STATUS_UIDNEXT, &status);

	str = t_str_new(256);
	str_printfa(str, "fl=uid,score&rows=%u&sort=uid+asc&q=",
		    status.uidnext);
	prefix_len = str_len(str);

	if (solr_add_definite_query_args(str, args, and_args)) {
		if (solr_search(_backend, str, box_guid,
				&result->definite_uids, &result->scores) < 0)
			return -1;
	}
	str_truncate(str, prefix_len);
	if (solr_add_maybe_query_args(str, args, and_args)) {
		if (solr_search(_backend, str, box_guid,
				&result->maybe_uids, &result->scores) < 0)
			return -1;
	}               
	result->scores_sorted = TRUE;
	return 0;
}

static int
solr_search_multi(struct fts_backend *_backend, string_t *str,
		  struct mailbox *const boxes[],
		  struct fts_multi_result *result)
{
	struct solr_result **solr_results;
	struct fts_result *fts_result;
	ARRAY_DEFINE(fts_results, struct fts_result);
	struct hash_table *mailboxes;
	struct mailbox *box;
	const char *box_guid;
	unsigned int i, len;

	/* use a separate filter query for selecting the mailbox. it shouldn't
	   affect the score and there could be some caching benefits too. */
	str_append(str, "&fq=%2Buser:");
	if (_backend->ns->owner != NULL)
		solr_quote_http(str, _backend->ns->owner->username);
	else
		str_append(str, "%22%22");

	mailboxes = hash_table_create(default_pool, default_pool, 0,
				      str_hash, (hash_cmp_callback_t *)strcmp);
	str_append(str, "%2B(");
	len = str_len(str);
	for (i = 0; boxes[i] != NULL; i++) {
		if (fts_mailbox_get_guid(boxes[i], &box_guid) < 0)
			continue;

		if (str_len(str) != len)
			str_append(str, "+OR+");
		str_printfa(str, "box:%s", box_guid);
		hash_table_insert(mailboxes, t_strdup_noconst(box_guid),
				  boxes[i]);
	}
	str_append_c(str, ')');

	if (solr_connection_select(solr_conn, str_c(str),
				   result->pool, &solr_results) < 0) {
		hash_table_destroy(&mailboxes);
		return -1;
	}

	p_array_init(&fts_results, result->pool, 32);
	for (i = 0; solr_results[i] != NULL; i++) {
		box = hash_table_lookup(mailboxes, solr_results[i]->box_id);
		if (box == NULL) {
			i_warning("fts_solr: Lookup returned unexpected mailbox "
				  "with guid=%s", solr_results[i]->box_id);
			continue;
		}
		fts_result = array_append_space(&fts_results);
		fts_result->box = box;
		fts_result->definite_uids = solr_results[i]->uids;
		fts_result->scores = solr_results[i]->scores;
		fts_result->scores_sorted = TRUE;
	}
	(void)array_append_space(&fts_results);
	result->box_results = array_idx_modifiable(&fts_results, 0);
	hash_table_destroy(&mailboxes);
	return 0;
}

static int
fts_backend_solr_lookup_multi(struct fts_backend *backend,
			      struct mailbox *const boxes[],
			      struct mail_search_arg *args, bool and_args,
			      struct fts_multi_result *result)
{
	string_t *str;

	str = t_str_new(256);
	str_printfa(str, "fl=box,uid,score&rows=%u&sort=box+asc,uid+asc&q=",
		    SOLR_MAX_MULTI_ROWS);

	if (solr_add_definite_query_args(str, args, and_args)) {
		if (solr_search_multi(backend, str, boxes, result) < 0)
			return -1;
	}
	/* FIXME: maybe_uids could be handled also with some more work.. */
	return 0;
}

struct fts_backend fts_backend_solr = {
	.name = "solr",
	.flags = 0,

	{
		fts_backend_solr_alloc,
		fts_backend_solr_init,
		fts_backend_solr_deinit,
		fts_backend_solr_get_last_uid,
		fts_backend_solr_update_init,
		fts_backend_solr_update_deinit,
		fts_backend_solr_update_set_mailbox,
		fts_backend_solr_update_expunge,
		fts_backend_solr_update_set_build_key,
		fts_backend_solr_update_unset_build_key,
		fts_backend_solr_update_build_more,
		fts_backend_solr_refresh,
		NULL,
		fts_backend_solr_optimize,
		fts_backend_default_can_lookup,
		fts_backend_solr_lookup,
		fts_backend_solr_lookup_multi,
		NULL
	}
};
