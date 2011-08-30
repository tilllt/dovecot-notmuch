/* Copyright (c) 2002-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "seq-range-array.h"
#include "ioloop.h"
#include "array.h"
#include "index-sync-private.h"

struct index_storage_list_index_record {
	uint32_t size;
	uint32_t mtime;
};

enum cache_mask {
	CACHE_HDR		= 0x01,
	CACHE_BODY		= 0x02,
	CACHE_RECEIVED_DATE	= 0x04,
	CACHE_SAVE_DATE		= 0x08,
	CACHE_VIRTUAL_SIZE	= 0x10,
	CACHE_PHYSICAL_SIZE	= 0x20,
	CACHE_POP3_UIDL		= 0x40,
	CACHE_GUID		= 0x80
};

enum mail_index_sync_flags index_storage_get_sync_flags(struct mailbox *box)
{
	enum mail_index_sync_flags sync_flags = 0;

	if ((box->flags & MAILBOX_FLAG_DROP_RECENT) != 0)
		sync_flags |= MAIL_INDEX_SYNC_FLAG_DROP_RECENT;
	if (box->deleting)
		sync_flags |= MAIL_INDEX_SYNC_FLAG_DELETING_INDEX;
	return sync_flags;
}

bool index_mailbox_want_full_sync(struct mailbox *box,
				  enum mailbox_sync_flags flags)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);

	if ((flags & MAILBOX_SYNC_FLAG_FAST) != 0 &&
	    ioloop_time < ibox->sync_last_check + MAILBOX_FULL_SYNC_INTERVAL)
		return FALSE;

	if (ibox->notify_to != NULL)
		timeout_reset(ibox->notify_to);
	ibox->sync_last_check = ioloop_time;
	return TRUE;
}

void index_mailbox_set_recent_uid(struct mailbox *box, uint32_t uid)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);

	if (uid <= ibox->recent_flags_prev_uid) {
		if (seq_range_exists(&ibox->recent_flags, uid))
			return;

		mail_storage_set_critical(box->storage,
			"Recent flags state corrupted for mailbox %s",
			box->vname);
		array_clear(&ibox->recent_flags);
		ibox->recent_flags_count = 0;
	}
	ibox->recent_flags_prev_uid = uid;

	seq_range_array_add(&ibox->recent_flags, 64, uid);
	ibox->recent_flags_count++;
}

void index_mailbox_set_recent_seq(struct mailbox *box,
				  struct mail_index_view *view,
				  uint32_t seq1, uint32_t seq2)
{
	uint32_t uid;

	for (; seq1 <= seq2; seq1++) {
		mail_index_lookup_uid(view, seq1, &uid);
		index_mailbox_set_recent_uid(box, uid);
	}
}

bool index_mailbox_is_recent(struct mailbox *box, uint32_t uid)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);

	return array_is_created(&ibox->recent_flags) &&
		seq_range_exists(&ibox->recent_flags, uid);
}

void index_mailbox_reset_uidvalidity(struct mailbox *box)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);

	/* can't trust the currently cached recent flags anymore */
	if (array_is_created(&ibox->recent_flags))
		array_clear(&ibox->recent_flags);
	ibox->recent_flags_count = 0;
	ibox->recent_flags_prev_uid = 0;
}

unsigned int index_mailbox_get_recent_count(struct mailbox *box)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);
	const struct mail_index_header *hdr;
	const struct seq_range *range;
	unsigned int i, count, recent_count;

	if (!array_is_created(&ibox->recent_flags))
		return 0;

	hdr = mail_index_get_header(box->view);
	recent_count = ibox->recent_flags_count;
	range = array_get(&ibox->recent_flags, &count);
	for (i = count; i > 0; ) {
		i--;
		if (range[i].seq2 < hdr->next_uid)
			break;

		if (range[i].seq1 >= hdr->next_uid) {
			/* completely invisible to this view */
			recent_count -= range[i].seq2 - range[i].seq1 + 1;
		} else {
			/* partially invisible */
			recent_count -= range[i].seq2 - hdr->next_uid + 1;
			break;
		}
	}
	return recent_count;
}

static void
index_mailbox_expunge_recent(struct mailbox *box, uint32_t seq1, uint32_t seq2)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);
	uint32_t uid;

	if (!array_is_created(&ibox->recent_flags))
		return;

	for (; seq1 <= seq2; seq1++) {
		mail_index_lookup_uid(box->view, seq1, &uid);
		if (seq_range_array_remove(&ibox->recent_flags, uid))
			ibox->recent_flags_count--;
	}
}

static void index_view_sync_recs_get(struct index_mailbox_sync_context *ctx)
{
	struct mail_index_view_sync_rec sync_rec;
	uint32_t seq1, seq2;

	i_array_init(&ctx->flag_updates, 128);
	i_array_init(&ctx->hidden_updates, 32);
	while (mail_index_view_sync_next(ctx->sync_ctx, &sync_rec)) {
		switch (sync_rec.type) {
		case MAIL_INDEX_VIEW_SYNC_TYPE_FLAGS:
			if (!mail_index_lookup_seq_range(ctx->ctx.box->view,
							 sync_rec.uid1,
							 sync_rec.uid2,
							 &seq1, &seq2))
				break;

			if (!sync_rec.hidden) {
				seq_range_array_add_range(&ctx->flag_updates,
							  seq1, seq2);
			} else if (array_is_created(&ctx->hidden_updates)) {
				seq_range_array_add_range(&ctx->hidden_updates,
							  seq1, seq2);
			}
			break;
		}
	}

	/* remove expunged messages from flag updates */
	if (ctx->expunges != NULL) {
		seq_range_array_remove_seq_range(&ctx->flag_updates,
						 ctx->expunges);
		seq_range_array_remove_seq_range(&ctx->hidden_updates,
						 ctx->expunges);
	}
	/* remove flag updates from hidden updates */
	seq_range_array_remove_seq_range(&ctx->hidden_updates,
					 &ctx->flag_updates);
}

struct mailbox_sync_context *
index_mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags,
			bool failed)
{
        struct index_mailbox_sync_context *ctx;
	enum mail_index_view_sync_flags sync_flags = 0;

	ctx = i_new(struct index_mailbox_sync_context, 1);
	ctx->ctx.box = box;
	ctx->ctx.flags = flags;

	if (failed) {
		ctx->failed = TRUE;
		return &ctx->ctx;
	}

	if ((flags & MAILBOX_SYNC_FLAG_NO_EXPUNGES) != 0)
		sync_flags |= MAIL_INDEX_VIEW_SYNC_FLAG_NOEXPUNGES;

	if ((flags & MAILBOX_SYNC_FLAG_FIX_INCONSISTENT) != 0) {
		sync_flags |= MAIL_INDEX_VIEW_SYNC_FLAG_FIX_INCONSISTENT;
		ctx->messages_count = 0;
	} else {
		ctx->messages_count =
			mail_index_view_get_messages_count(box->view);
	}

	ctx->sync_ctx = mail_index_view_sync_begin(box->view, sync_flags);
	if ((flags & MAILBOX_SYNC_FLAG_NO_EXPUNGES) == 0) {
		mail_index_view_sync_get_expunges(ctx->sync_ctx,
						  &ctx->expunges);
		ctx->expunge_pos = array_count(ctx->expunges);
	}
	index_view_sync_recs_get(ctx);
	index_sync_search_results_expunge(ctx);
	return &ctx->ctx;
}

static bool
index_mailbox_sync_next_expunge(struct index_mailbox_sync_context *ctx,
				struct mailbox_sync_rec *sync_rec_r)
{
	const struct seq_range *range;

	if (ctx->expunge_pos == 0)
		return FALSE;

	/* expunges is a sorted array of sequences. it's easiest for
	   us to print them from end to beginning. */
	ctx->expunge_pos--;
	range = array_idx(ctx->expunges, ctx->expunge_pos);
	i_assert(range->seq2 <= ctx->messages_count);

	index_mailbox_expunge_recent(ctx->ctx.box, range->seq1, range->seq2);
	ctx->messages_count -= range->seq2 - range->seq1 + 1;

	sync_rec_r->seq1 = range->seq1;
	sync_rec_r->seq2 = range->seq2;
	sync_rec_r->type = MAILBOX_SYNC_TYPE_EXPUNGE;
	return TRUE;
}

bool index_mailbox_sync_next(struct mailbox_sync_context *_ctx,
			     struct mailbox_sync_rec *sync_rec_r)
{
	struct index_mailbox_sync_context *ctx =
		(struct index_mailbox_sync_context *)_ctx;
	const struct seq_range *range;
	unsigned int count;

	if (ctx->failed)
		return FALSE;

	range = array_get(&ctx->flag_updates, &count);
	if (ctx->flag_update_idx < count) {
		sync_rec_r->type = MAILBOX_SYNC_TYPE_FLAGS;
		sync_rec_r->seq1 = range[ctx->flag_update_idx].seq1;
		sync_rec_r->seq2 = range[ctx->flag_update_idx].seq2;
		ctx->flag_update_idx++;
		return TRUE;
	}
	if ((_ctx->box->enabled_features & MAILBOX_FEATURE_CONDSTORE) != 0) {
		/* hidden flag changes' MODSEQs still need to be returned */
		range = array_get(&ctx->hidden_updates, &count);
		if (ctx->hidden_update_idx < count) {
			sync_rec_r->type = MAILBOX_SYNC_TYPE_MODSEQ;
			sync_rec_r->seq1 = range[ctx->hidden_update_idx].seq1;
			sync_rec_r->seq2 = range[ctx->hidden_update_idx].seq2;
			ctx->hidden_update_idx++;
			return TRUE;
		}
	}

	return index_mailbox_sync_next_expunge(ctx, sync_rec_r);
}

static void
index_mailbox_expunge_unseen_recent(struct index_mailbox_sync_context *ctx)
{
	struct index_mailbox_context *ibox =
		INDEX_STORAGE_CONTEXT(ctx->ctx.box);
	struct mail_index_view *view = ctx->ctx.box->view;
	const struct mail_index_header *hdr;
	uint32_t seq, start_uid, uid;

	if (!array_is_created(&ibox->recent_flags))
		return;

	/* expunges array contained expunges for the messages that were already
	   visible in this view, but append+expunge would be invisible.
	   recent_flags may however contain the append UID, so we'll have to
	   remove it separately */
	hdr = mail_index_get_header(view);
	if (ctx->messages_count == 0)
		uid = 0;
	else if (ctx->messages_count <= hdr->messages_count)
		mail_index_lookup_uid(view, ctx->messages_count, &uid);
	else {
		i_assert(mail_index_view_is_inconsistent(view));
		return;
	}

	for (seq = ctx->messages_count + 1; seq <= hdr->messages_count; seq++) {
		start_uid = uid;
		mail_index_lookup_uid(view, seq, &uid);
		if (start_uid + 1 > uid - 1)
			continue;

		ibox->recent_flags_count -=
			seq_range_array_remove_range(&ibox->recent_flags,
						     start_uid + 1, uid - 1);
	}

	if (uid + 1 < hdr->next_uid) {
		ibox->recent_flags_count -=
			seq_range_array_remove_range(&ibox->recent_flags,
						     uid + 1,
						     hdr->next_uid - 1);
	}
#ifdef DEBUG
	if (!mail_index_view_is_inconsistent(view)) {
		const struct seq_range *range;
		unsigned int i, count;

		range = array_get(&ibox->recent_flags, &count);
		for (i = 0; i < count; i++) {
			for (uid = range[i].seq1; uid <= range[i].seq2; uid++) {
				if (uid >= hdr->next_uid)
					break;
				mail_index_lookup_seq(view, uid, &seq);
				i_assert(seq != 0);
			}
		}
	}
#endif
}

static enum cache_mask
cache_fields_get(const struct mailbox_metadata *metadata, bool debug)
{
	const char *const *cache_fields;
	unsigned int i, count;
	enum cache_mask cache = 0;

	cache_fields = array_get(metadata->cache_fields, &count);
	for (i = 0; i < count; i++) {
		if (strncmp(cache_fields[i], "hdr.", 4) == 0 ||
		    strcmp(cache_fields[i], "date.sent") == 0 ||
		    strcmp(cache_fields[i], "imap.envelope") == 0)
			cache |= CACHE_HDR;
		else if (strcmp(cache_fields[i], "mime.parts") == 0 ||
			 strcmp(cache_fields[i], "imap.body") == 0 ||
			 strcmp(cache_fields[i], "imap.bodystructure") == 0)
			cache |= CACHE_BODY;
		else if (strcmp(cache_fields[i], "date.received") == 0)
			cache |= CACHE_RECEIVED_DATE;
		else if (strcmp(cache_fields[i], "date.save") == 0)
			cache |= CACHE_SAVE_DATE;
		else if (strcmp(cache_fields[i], "size.virtual") == 0)
			cache |= CACHE_VIRTUAL_SIZE;
		else if (strcmp(cache_fields[i], "size.physical") == 0)
			cache |= CACHE_PHYSICAL_SIZE;
		else if (strcmp(cache_fields[i], "pop3.uidl") == 0)
			cache |= CACHE_POP3_UIDL;
		else if (strcmp(cache_fields[i], "guid") == 0)
			cache |= CACHE_GUID;
		else if (debug) {
			i_debug("Ignoring unknown cache field: %s",
				cache_fields[i]);
		}
	}
	return cache;
}

static int cache_add(struct mailbox *box, enum cache_mask cache)
{
	struct mailbox_status status;
	struct mailbox_transaction_context *trans;
	struct mail *mail;
	uint32_t seq;
	time_t date;
	uoff_t size;
	const char *str;

	if (cache == 0) {
		if (box->storage->set->mail_debug) {
			i_debug("%s: Nothing in mailbox cache, skipping",
				box->vname);
		}
		return 0;
	}

	/* find the first message we need to index */
	mailbox_get_open_status(box, STATUS_MESSAGES, &status);
	trans = mailbox_transaction_begin(box, MAILBOX_TRANSACTION_FLAG_NO_CACHE_DEC);
	mail = mail_alloc(trans, 0, NULL);
	for (seq = status.messages; seq > 0; seq--) {
		mail_set_seq(mail, seq);
		if (mail_is_cached(mail))
			break;
	}
	seq++;

	if (box->storage->set->mail_debug) {
		if (seq > status.messages) {
			i_debug("%s: Cache is already up to date", box->vname);
		} else {
			i_debug("%s: Caching mails seq=%u..%u cache=0x%x",
				box->vname, seq, status.messages, cache);
		}
	}

	for (; seq <= status.messages; seq++) {
		mail_set_seq(mail, seq);

		if ((cache & (CACHE_HDR | CACHE_BODY)) != 0)
			mail_parse(mail, (cache & CACHE_BODY) != 0);
		if ((cache & CACHE_RECEIVED_DATE) != 0)
			(void)mail_get_received_date(mail, &date);
		if ((cache & CACHE_SAVE_DATE) != 0)
			(void)mail_get_save_date(mail, &date);
		if ((cache & CACHE_VIRTUAL_SIZE) != 0)
			(void)mail_get_virtual_size(mail, &size);
		if ((cache & CACHE_PHYSICAL_SIZE) != 0)
			(void)mail_get_physical_size(mail, &size);
		if ((cache & CACHE_POP3_UIDL) != 0) {
			(void)mail_get_special(mail, MAIL_FETCH_UIDL_BACKEND,
					       &str);
		}
		if ((cache & CACHE_GUID) != 0)
			(void)mail_get_special(mail, MAIL_FETCH_GUID, &str);
	}
	mail_free(&mail);
	if (mailbox_transaction_commit(&trans) < 0) {
		mail_storage_set_critical(box->storage,
			"Commiting mailbox %s failed: %s", box->vname,
			mailbox_get_last_error(box, NULL));
		return -1;
	}
	return 0;
}

static int index_sync_precache(struct mailbox *box)
{
	struct mailbox_metadata metadata;
	enum cache_mask cache;

	if (mailbox_get_metadata(box, MAILBOX_METADATA_CACHE_FIELDS,
				 &metadata) < 0) {
		mail_storage_set_critical(box->storage,
			"Metadata lookup from mailbox %s failed: %s", box->vname,
			mailbox_get_last_error(box, NULL));
		return -1;
	}

	cache = cache_fields_get(&metadata, box->storage->set->mail_debug);
	return cache_add(box, cache);
}

void index_sync_update_recent_count(struct mailbox *box)
{
	struct index_mailbox_context *ibox = INDEX_STORAGE_CONTEXT(box);
	const struct mail_index_header *hdr;
	uint32_t seq1, seq2;

	hdr = mail_index_get_header(box->view);
	if (hdr->first_recent_uid > ibox->recent_flags_prev_uid) {
		mail_index_lookup_seq_range(box->view,
					    hdr->first_recent_uid,
					    hdr->next_uid,
					    &seq1, &seq2);
		if (seq1 != 0) {
			index_mailbox_set_recent_seq(box, box->view,
						     seq1, seq2);
		}
	}
}

int index_mailbox_sync_deinit(struct mailbox_sync_context *_ctx,
			      struct mailbox_sync_status *status_r)
{
	struct index_mailbox_sync_context *ctx =
		(struct index_mailbox_sync_context *)_ctx;
	struct mailbox_sync_rec sync_rec;
	bool delayed_expunges = FALSE;
	int ret = ctx->failed ? -1 : 0;

	/* finish handling expunges, so we don't break when updating
	   recent flags */
	while (index_mailbox_sync_next_expunge(ctx, &sync_rec) > 0) ;

	/* convert sequences to uids before syncing view */
	index_sync_search_results_uidify(ctx);

	if (ctx->sync_ctx != NULL) {
		if (mail_index_view_sync_commit(&ctx->sync_ctx,
						&delayed_expunges) < 0) {
			mail_storage_set_index_error(_ctx->box);
			ret = -1;
		}
	}
	index_mailbox_expunge_unseen_recent(ctx);

	if ((_ctx->box->flags & MAILBOX_FLAG_DROP_RECENT) == 0 &&
	    _ctx->box->opened) {
		/* mailbox syncing didn't necessarily update our recent state */
		index_sync_update_recent_count(_ctx->box);
	}

	if (status_r != NULL)
		status_r->sync_delayed_expunges = delayed_expunges;

	index_sync_search_results_update(ctx);

	if (array_is_created(&ctx->flag_updates))
		array_free(&ctx->flag_updates);
	if (array_is_created(&ctx->hidden_updates))
		array_free(&ctx->hidden_updates);
	if (array_is_created(&ctx->all_flag_update_uids))
		array_free(&ctx->all_flag_update_uids);

	if ((_ctx->flags & MAILBOX_SYNC_FLAG_PRECACHE) != 0 && ret == 0) {
		if (index_sync_precache(_ctx->box) < 0)
			ret = -1;
	}
	i_free(ctx);
	return ret;
}

bool index_keyword_array_cmp(const ARRAY_TYPE(keyword_indexes) *k1,
			     const ARRAY_TYPE(keyword_indexes) *k2)
{
	const unsigned int *idx1, *idx2;
	unsigned int i, j, count1, count2;

	if (!array_is_created(k1))
		return !array_is_created(k2) || array_count(k2) == 0;
	if (!array_is_created(k2))
		return array_count(k1) == 0;

	/* The arrays may not be sorted, but they usually are. Optimize for
	   the assumption that they are */
	idx1 = array_get(k1, &count1);
	idx2 = array_get(k2, &count2);

	if (count1 != count2)
		return FALSE;

	for (i = 0; i < count1; i++) {
		if (idx1[i] != idx2[i]) {
			/* not found / unsorted array. check. */
			for (j = 0; j < count1; j++) {
				if (idx1[i] == idx2[j])
					break;
			}
			if (j == count1)
				return FALSE;
		}
	}
	return TRUE;
}

enum mailbox_sync_type index_sync_type_convert(enum mail_index_sync_type type)
{
	enum mailbox_sync_type ret = 0;

	if ((type & MAIL_INDEX_SYNC_TYPE_EXPUNGE) != 0)
		ret |= MAILBOX_SYNC_TYPE_EXPUNGE;
	if ((type & (MAIL_INDEX_SYNC_TYPE_FLAGS |
		     MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD |
		     MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE |
		     MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET)) != 0)
		ret |= MAILBOX_SYNC_TYPE_FLAGS;
	return ret;
}

static unsigned int
index_storage_list_get_ext_id(struct mail_storage *storage,
			      struct mail_index_view *view)
{
	if (storage->list_sync_ext_id == (uint32_t)-1) {
		storage->list_sync_ext_id =
			mail_index_ext_register(mail_index_view_get_index(view),
				"index sync", 0,
				sizeof(struct index_storage_list_index_record),
				sizeof(uint32_t));
	}
	return storage->list_sync_ext_id;
}

int index_storage_list_index_has_changed(struct mailbox *box,
					 struct mail_index_view *list_view,
					 uint32_t seq)
{
	const struct index_storage_list_index_record *rec;
	const void *data;
	const char *dir, *path;
	struct stat st;
	uint32_t ext_id;
	bool expunged;

	if (mail_index_is_in_memory(mail_index_view_get_index(list_view)))
		return 1;

	ext_id = index_storage_list_get_ext_id(box->storage, list_view);
	mail_index_lookup_ext(list_view, seq, ext_id, &data, &expunged);
	rec = data;

	if (rec == NULL || expunged || rec->size == 0 || rec->mtime == 0) {
		/* doesn't exist / not synced */
		return 1;
	}

	dir = mailbox_list_get_path(box->list, box->name,
				    MAILBOX_LIST_PATH_TYPE_INDEX);
	path = t_strconcat(dir, "/", box->index_prefix, ".log", NULL);
	if (stat(path, &st) < 0) {
		mail_storage_set_critical(box->storage,
					  "stat(%s) failed: %m", path);
		return -1;
	}
	if (rec->size != (st.st_size & 0xffffffffU) ||
	    rec->mtime != (st.st_mtime & 0xffffffffU))
		return 1;
	return 0;
}

void index_storage_list_index_update_sync(struct mailbox *box,
					  struct mail_index_transaction *trans,
					  uint32_t seq)
{
	struct mail_index_view *list_view;
	const struct index_storage_list_index_record *old_rec;
	struct index_storage_list_index_record new_rec;
	const void *data;
	const char *dir, *path;
	struct stat st;
	uint32_t ext_id;
	bool expunged;

	list_view = mail_index_transaction_get_view(trans);
	if (mail_index_is_in_memory(mail_index_view_get_index(list_view)))
		return;

	/* get the current record */
	ext_id = index_storage_list_get_ext_id(box->storage, list_view);
	mail_index_lookup_ext(list_view, seq, ext_id, &data, &expunged);
	if (expunged)
		return;
	old_rec = data;

	dir = mailbox_list_get_path(box->list, box->name,
				    MAILBOX_LIST_PATH_TYPE_INDEX);
	path = t_strconcat(dir, "/", box->index_prefix, ".log", NULL);
	if (stat(path, &st) < 0) {
		mail_storage_set_critical(box->storage,
					  "stat(%s) failed: %m", path);
		return;
	}

	memset(&new_rec, 0, sizeof(new_rec));
	new_rec.size = st.st_size & 0xffffffffU;
	new_rec.mtime = st.st_mtime & 0xffffffffU;

	if (old_rec == NULL ||
	    memcmp(old_rec, &new_rec, sizeof(*old_rec)) != 0) {
		mail_index_update_ext(trans, seq,
				      box->storage->list_sync_ext_id,
				      &new_rec, NULL);
	}
}
