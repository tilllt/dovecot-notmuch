/* Copyright (c) 2002-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "ioloop.h"
#include "istream.h"
#include "str.h"
#include "message-date.h"
#include "message-part-serialize.h"
#include "message-parser.h"
#include "imap-bodystructure.h"
#include "imap-envelope.h"
#include "mail-cache.h"
#include "mail-index-modseq.h"
#include "index-storage.h"
#include "istream-mail.h"
#include "index-mail.h"

#include <fcntl.h>

struct mail_cache_field global_cache_fields[MAIL_INDEX_CACHE_FIELD_COUNT] = {
	{ .name = "flags",
	  .type = MAIL_CACHE_FIELD_BITMASK,
	  .field_size = sizeof(uint32_t) },
	{ .name = "date.sent",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(struct mail_sent_date) },
	{ .name = "date.received",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(uint32_t) },
	{ .name = "date.save",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(uint32_t) },
	{ .name = "size.virtual",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(uoff_t) },
	{ .name = "size.physical",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(uoff_t) },
	{ .name = "imap.body",
	  .type = MAIL_CACHE_FIELD_STRING },
	{ .name = "imap.bodystructure",
	  .type = MAIL_CACHE_FIELD_STRING },
	{ .name = "imap.envelope",
	  .type = MAIL_CACHE_FIELD_STRING },
	{ .name = "pop3.uidl",
	  .type = MAIL_CACHE_FIELD_STRING },
	{ .name = "pop3.order",
	  .type = MAIL_CACHE_FIELD_FIXED_SIZE,
	  .field_size = sizeof(uint32_t) },
	{ .name = "guid",
	  .type = MAIL_CACHE_FIELD_STRING },
	{ .name = "mime.parts",
	  .type = MAIL_CACHE_FIELD_VARIABLE_SIZE },
	{ .name = "binary.parts",
	  .type = MAIL_CACHE_FIELD_VARIABLE_SIZE }
};

static int index_mail_parse_body(struct index_mail *mail,
				 enum index_cache_field field);

int index_mail_cache_lookup_field(struct index_mail *mail, buffer_t *buf,
				  unsigned int field_idx)
{
	int ret;

	ret = mail_cache_lookup_field(mail->mail.mail.transaction->cache_view,
				      buf, mail->data.seq, field_idx);
	if (ret > 0)
		mail->mail.mail.transaction->stats.cache_hit_count++;
	return ret;
}

static struct message_part *get_unserialized_parts(struct index_mail *mail)
{
	const unsigned int field_idx =
		mail->ibox->cache_fields[MAIL_CACHE_MESSAGE_PARTS].idx;
	struct message_part *parts;
	buffer_t *part_buf;
	const char *error;
	int ret;

	part_buf = buffer_create_dynamic(pool_datastack_create(), 128);
	ret = index_mail_cache_lookup_field(mail, part_buf, field_idx);
	if (ret <= 0)
		return NULL;

	parts = message_part_deserialize(mail->mail.data_pool, part_buf->data,
					 part_buf->used, &error);
	if (parts == NULL) {
		mail_cache_set_corrupted(mail->mail.mail.box->cache,
			"Corrupted cached mime.parts data (%s)", error);
	}
	return parts;
}

static bool message_parts_have_nuls(const struct message_part *part)
{
	for (; part != NULL; part = part->next) {
		if ((part->flags & MESSAGE_PART_FLAG_HAS_NULS) != 0)
			return TRUE;
		if (part->children != NULL) {
			if (message_parts_have_nuls(part->children))
				return TRUE;
		}
	}
	return FALSE;
}

static bool get_cached_parts(struct index_mail *mail)
{
	struct message_part *part;

	T_BEGIN {
		part = get_unserialized_parts(mail);
	} T_END;
	if (part == NULL)
		return FALSE;

	/* we know the NULs now, update them */
	if (message_parts_have_nuls(part)) {
		mail->mail.mail.has_nuls = TRUE;
		mail->mail.mail.has_no_nuls = FALSE;
	} else {
		mail->mail.mail.has_nuls = FALSE;
		mail->mail.mail.has_no_nuls = TRUE;
	}

	mail->data.parts = part;
	return TRUE;
}

static bool index_mail_get_fixed_field(struct index_mail *mail,
				       enum index_cache_field field,
				       void *data, size_t data_size)
{
	const unsigned int field_idx = mail->ibox->cache_fields[field].idx;
	buffer_t buf;
	int ret;

	buffer_create_from_data(&buf, data, data_size);
	if (index_mail_cache_lookup_field(mail, &buf, field_idx) <= 0)
		ret = FALSE;
	else {
		i_assert(buf.used == data_size);
		ret = TRUE;
	}
	return ret;
}

bool index_mail_get_cached_uoff_t(struct index_mail *mail,
				  enum index_cache_field field, uoff_t *size_r)
{
	return index_mail_get_fixed_field(mail, field,
					  size_r, sizeof(*size_r));
}

static bool index_mail_get_pvt(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;

	if (mail->seq_pvt != 0)
		return TRUE;
	if (_mail->box->view_pvt == NULL) {
		/* no private view (set by view syncing) -> no private flags */
		return FALSE;
	}
	if (_mail->saving) {
		/* mail is still being saved, it has no private flags yet */
		return FALSE;
	}
	i_assert(_mail->uid != 0);

	index_transaction_init_pvt(_mail->transaction);
	if (!mail_index_lookup_seq(_mail->transaction->view_pvt, _mail->uid,
				   &mail->seq_pvt))
		mail->seq_pvt = 0;
	return mail->seq_pvt != 0;
}

enum mail_flags index_mail_get_flags(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	const struct mail_index_record *rec;
	enum mail_flags flags, pvt_flags_mask;

	rec = mail_index_lookup(_mail->transaction->view, _mail->seq);
	flags = rec->flags & (MAIL_FLAGS_NONRECENT |
			      MAIL_INDEX_MAIL_FLAG_BACKEND);

	if (index_mailbox_is_recent(_mail->box, _mail->uid))
		flags |= MAIL_RECENT;

	if (index_mail_get_pvt(_mail)) {
		/* mailbox has private flags */
		pvt_flags_mask = mailbox_get_private_flags_mask(_mail->box);
		flags &= ~pvt_flags_mask;
		rec = mail_index_lookup(_mail->transaction->view_pvt,
					mail->seq_pvt);
		flags |= rec->flags & pvt_flags_mask;
	}
	return flags;
}

uint64_t index_mail_get_modseq(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	if (mail->data.modseq != 0)
		return mail->data.modseq;

	mail_index_modseq_enable(_mail->box->index);
	mail->data.modseq =
		mail_index_modseq_lookup(_mail->transaction->view, _mail->seq);
	return mail->data.modseq;
}

uint64_t index_mail_get_pvt_modseq(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	if (mail->data.pvt_modseq != 0)
		return mail->data.pvt_modseq;

	if (mailbox_open_index_pvt(_mail->box) <= 0)
		return 0;
	index_transaction_init_pvt(_mail->transaction);

	mail_index_modseq_enable(_mail->box->index_pvt);
	mail->data.pvt_modseq =
		mail_index_modseq_lookup(_mail->transaction->view_pvt,
					 _mail->seq);
	return mail->data.pvt_modseq;
}

const char *const *index_mail_get_keywords(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	const char *const *names;
	const unsigned int *keyword_indexes;
	unsigned int i, count, names_count;

	if (array_is_created(&data->keywords))
		return array_idx(&data->keywords, 0);

	(void)index_mail_get_keyword_indexes(_mail);

	keyword_indexes = array_get(&data->keyword_indexes, &count);
	names = array_get(mail->ibox->keyword_names, &names_count);
	p_array_init(&data->keywords, mail->mail.data_pool, count + 1);
	for (i = 0; i < count; i++) {
		const char *name;
		i_assert(keyword_indexes[i] < names_count);

		name = names[keyword_indexes[i]];
		array_append(&data->keywords, &name, 1);
	}

	/* end with NULL */
	array_append_zero(&data->keywords);
	return array_idx(&data->keywords, 0);
}

const ARRAY_TYPE(keyword_indexes) *
index_mail_get_keyword_indexes(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;

	if (!array_is_created(&data->keyword_indexes)) {
		p_array_init(&data->keyword_indexes, mail->mail.data_pool, 32);
		mail_index_lookup_keywords(_mail->transaction->view,
					   mail->data.seq,
					   &data->keyword_indexes);
	}
	return &data->keyword_indexes;
}

int index_mail_get_parts(struct mail *_mail, struct message_part **parts_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;

	data->cache_fetch_fields |= MAIL_FETCH_MESSAGE_PARTS;
	if (data->parts != NULL || get_cached_parts(mail)) {
		*parts_r = data->parts;
		return 0;
	}

	if (data->parser_ctx == NULL) {
		if (index_mail_parse_headers(mail, NULL) < 0)
			return -1;
	}

	data->save_message_parts = TRUE;
	if (index_mail_parse_body(mail, 0) < 0)
		return -1;

	*parts_r = data->parts;
	return 0;
}

int index_mail_get_received_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;

	data->cache_fetch_fields |= MAIL_FETCH_RECEIVED_DATE;
	if (data->received_date == (time_t)-1) {
		uint32_t t;

		if (index_mail_get_fixed_field(mail, MAIL_CACHE_RECEIVED_DATE,
						&t, sizeof(t)))
			data->received_date = t;
	}

	*date_r = data->received_date;
	return *date_r == (time_t)-1 ? -1 : 0;
}

int index_mail_get_save_date(struct mail *_mail, time_t *date_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;

	data->cache_fetch_fields |= MAIL_FETCH_SAVE_DATE;
	if (data->save_date == (time_t)-1) {
		uint32_t t;

		if (index_mail_get_fixed_field(mail, MAIL_CACHE_SAVE_DATE,
					       &t, sizeof(t)))
			data->save_date = t;
	}

	*date_r = data->save_date;
	return *date_r == (time_t)-1 ? -1 : 0;
}

static int index_mail_cache_sent_date(struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;
	const char *str;
	time_t t;
	int ret, tz;

	if (data->sent_date.time != (uint32_t)-1)
		return 0;

	if ((ret = mail_get_first_header(&mail->mail.mail, "Date", &str)) < 0)
		return ret;

	if (ret == 0 ||
	    !message_date_parse((const unsigned char *)str,
				strlen(str), &t, &tz)) {
		/* 0 = not found / invalid */
		t = 0;
		tz = 0;
	}
	data->sent_date.time = t;
	data->sent_date.timezone = tz;
	index_mail_cache_add(mail, MAIL_CACHE_SENT_DATE,
			     &data->sent_date, sizeof(data->sent_date));
	return 0;
}

int index_mail_get_date(struct mail *_mail, time_t *date_r, int *timezone_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mail_sent_date sentdate;

	data->cache_fetch_fields |= MAIL_FETCH_DATE;
	if (data->sent_date.time != (uint32_t)-1) {
		*timezone_r = data->sent_date.timezone;
		*date_r = data->sent_date.time;
		return 0;
	}

	if (index_mail_get_fixed_field(mail, MAIL_CACHE_SENT_DATE,
				       &sentdate, sizeof(sentdate)))
		data->sent_date = sentdate;

	if (index_mail_cache_sent_date(mail) < 0)
		return -1;

	*timezone_r = data->sent_date.timezone;
	*date_r = data->sent_date.time;
	return 0;
}

static bool get_cached_msgpart_sizes(struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;

	if (data->parts == NULL)
		(void)get_cached_parts(mail);

	if (data->parts != NULL) {
		data->hdr_size_set = TRUE;
		data->hdr_size = data->parts->header_size;
		data->body_size = data->parts->body_size;
		data->body_size_set = TRUE;
		data->virtual_size = data->parts->header_size.virtual_size +
			data->body_size.virtual_size;
		data->physical_size = data->parts->header_size.physical_size +
			data->body_size.physical_size;
	}

	return data->parts != NULL;
}

bool index_mail_get_cached_virtual_size(struct index_mail *mail, uoff_t *size_r)
{
	struct index_mail_data *data = &mail->data;
	uoff_t size;

	data->cache_fetch_fields |= MAIL_FETCH_VIRTUAL_SIZE;
	if (data->virtual_size == (uoff_t)-1) {
		if (index_mail_get_cached_uoff_t(mail,
						 MAIL_CACHE_VIRTUAL_FULL_SIZE,
						 &size))
			data->virtual_size = size;
		else {
			if (!get_cached_msgpart_sizes(mail))
				return FALSE;
		}
	}
	if (data->hdr_size_set && data->physical_size != (uoff_t)-1) {
		data->body_size.physical_size = data->physical_size -
			data->hdr_size.physical_size;
		data->body_size.virtual_size = data->virtual_size -
			data->hdr_size.virtual_size;
		data->body_size_set = TRUE;
	}
	*size_r = data->virtual_size;
	return TRUE;
}

static void index_mail_get_cached_body_size(struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;
	uoff_t tmp;

	if (!data->hdr_size_set)
		return;

	/* we've already called get_cached_msgpart_sizes() and it didn't work.
	   try to do this by using cached virtual size and a quick physical
	   size lookup. */
	if (!index_mail_get_cached_virtual_size(mail, &tmp))
		return;

	if (!data->body_size_set) {
		enum mail_lookup_abort old_abort = mail->mail.mail.lookup_abort;

		/* get the physical size, but not if it requires reading
		   through the whole message */
		if (mail->mail.mail.lookup_abort < MAIL_LOOKUP_ABORT_READ_MAIL)
			mail->mail.mail.lookup_abort = MAIL_LOOKUP_ABORT_READ_MAIL;
		if (mail_get_physical_size(&mail->mail.mail, &tmp) == 0) {
			/* we should have everything now. try again. */
			(void)index_mail_get_cached_virtual_size(mail, &tmp);
		}
		mail->mail.mail.lookup_abort = old_abort;
	}
}

int index_mail_get_virtual_size(struct mail *_mail, uoff_t *size_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct message_size hdr_size, body_size;
	struct istream *input;
	uoff_t old_offset;

	if (index_mail_get_cached_virtual_size(mail, size_r))
		return 0;

	old_offset = data->stream == NULL ? 0 : data->stream->v_offset;
	if (mail_get_stream(_mail, &hdr_size, &body_size, &input) < 0)
		return -1;
	i_stream_seek(data->stream, old_offset);

	i_assert(data->virtual_size != (uoff_t)-1);
	*size_r = data->virtual_size;
	return 0;
}

int index_mail_get_physical_size(struct mail *_mail, uoff_t *size_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	uoff_t size;

	data->cache_fetch_fields |= MAIL_FETCH_PHYSICAL_SIZE;
	if (data->physical_size == (uoff_t)-1) {
		if (index_mail_get_cached_uoff_t(mail,
						 MAIL_CACHE_PHYSICAL_FULL_SIZE,
						 &size))
			data->physical_size = size;
		else
			(void)get_cached_msgpart_sizes(mail);
	}
	*size_r = data->physical_size;
	return *size_r == (uoff_t)-1 ? -1 : 0;
}

void index_mail_cache_add(struct index_mail *mail, enum index_cache_field field,
			  const void *data, size_t data_size)
{
	index_mail_cache_add_idx(mail, mail->ibox->cache_fields[field].idx,
				 data, data_size);
}

void index_mail_cache_add_idx(struct index_mail *mail, unsigned int field_idx,
			      const void *data, size_t data_size)
{
	struct mail *_mail = &mail->mail.mail;
	const struct mail_storage_settings *set = _mail->box->storage->set;
	const struct mail_index_header *hdr;

	if (set->mail_cache_min_mail_count > 0) {
		/* First check if we've configured caching not to be used with
		   low enough message count. */
		hdr = mail_index_get_header(_mail->transaction->view);
		if (hdr->messages_count < set->mail_cache_min_mail_count)
			return;
	}

	if (!mail->data.no_caching &&
	    mail->data.dont_cache_field_idx != field_idx &&
	    !_mail->box->mail_cache_disabled) {
		mail_cache_add(_mail->transaction->cache_trans, _mail->seq,
			       field_idx, data, data_size);
	}
}

static void parse_bodystructure_part_header(struct message_part *part,
					    struct message_header_line *hdr,
					    pool_t pool)
{
	imap_bodystructure_parse_header(pool, part, hdr);
}

static bool want_plain_bodystructure_cached(struct index_mail *mail)
{
	const unsigned int cache_field_body =
		mail->ibox->cache_fields[MAIL_CACHE_IMAP_BODY].idx;
	const unsigned int cache_field_bodystructure =
		mail->ibox->cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;
	struct mail *_mail = &mail->mail.mail;

	if ((mail->data.wanted_fields & (MAIL_FETCH_IMAP_BODY |
					 MAIL_FETCH_IMAP_BODYSTRUCTURE)) != 0)
		return TRUE;

	if (mail_cache_field_want_add(_mail->transaction->cache_trans,
				      _mail->seq, cache_field_body))
		return TRUE;
	if (mail_cache_field_want_add(_mail->transaction->cache_trans,
				      _mail->seq, cache_field_bodystructure))
		return TRUE;
	return FALSE;
}

static void index_mail_body_parsed_cache_flags(struct index_mail *mail)
{
	struct mail *_mail = &mail->mail.mail;
	struct index_mail_data *data = &mail->data;
	unsigned int cache_flags_idx;
	uint32_t cache_flags = data->cache_flags;
	bool want_cached;

	cache_flags_idx = mail->ibox->cache_fields[MAIL_CACHE_FLAGS].idx;
	want_cached = mail_cache_field_want_add(_mail->transaction->cache_trans,
						_mail->seq, cache_flags_idx);

	if (data->parsed_bodystructure &&
	    imap_bodystructure_is_plain_7bit(data->parts) &&
	    (want_cached || want_plain_bodystructure_cached(mail))) {
		cache_flags |= MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII;
		/* we need message_parts cached to be able to
		   actually use it in BODY/BODYSTRUCTURE reply */
		want_cached = TRUE;
		data->save_message_parts = TRUE;
	}

	/* cache flags should never get unset as long as the message doesn't
	   change, but try to handle it anyway */
	cache_flags &= ~(MAIL_CACHE_FLAG_BINARY_HEADER |
			 MAIL_CACHE_FLAG_BINARY_BODY |
			 MAIL_CACHE_FLAG_HAS_NULS |
			 MAIL_CACHE_FLAG_HAS_NO_NULS);
	if (message_parts_have_nuls(data->parts)) {
		_mail->has_nuls = TRUE;
		_mail->has_no_nuls = FALSE;
		cache_flags |= MAIL_CACHE_FLAG_HAS_NULS;
	} else {
		_mail->has_nuls = FALSE;
		_mail->has_no_nuls = TRUE;
		cache_flags |= MAIL_CACHE_FLAG_HAS_NO_NULS;
	}

	if (data->hdr_size.virtual_size == data->hdr_size.physical_size)
		cache_flags |= MAIL_CACHE_FLAG_BINARY_HEADER;
	if (data->body_size.virtual_size == data->body_size.physical_size)
		cache_flags |= MAIL_CACHE_FLAG_BINARY_BODY;

	if (cache_flags != data->cache_flags && want_cached) {
		index_mail_cache_add_idx(mail, cache_flags_idx,
					 &cache_flags, sizeof(cache_flags));
	}
	data->cache_flags = cache_flags;
}

static void index_mail_body_parsed_cache_message_parts(struct index_mail *mail)
{
	struct mail *_mail = &mail->mail.mail;
	struct index_mail_data *data = &mail->data;
	const unsigned int cache_field =
		mail->ibox->cache_fields[MAIL_CACHE_MESSAGE_PARTS].idx;
	enum mail_cache_decision_type decision;
	buffer_t *buffer;

	if (data->messageparts_saved_to_cache ||
	    mail_cache_field_exists(_mail->transaction->cache_view, _mail->seq,
				    cache_field) != 0) {
		/* already cached */
		return;
	}

	decision = mail_cache_field_get_decision(_mail->box->cache,
						 cache_field);
	if (decision == (MAIL_CACHE_DECISION_NO | MAIL_CACHE_DECISION_FORCED)) {
		/* we never want it cached */
		return;
	}
	if (decision == MAIL_CACHE_DECISION_NO &&
	    !data->save_message_parts &&
	    (data->wanted_fields & MAIL_FETCH_MESSAGE_PARTS) == 0) {
		/* we didn't really care about the message parts themselves,
		   just wanted to use something that depended on it */
		return;
	}

	T_BEGIN {
		buffer = buffer_create_dynamic(pool_datastack_create(), 1024);
		message_part_serialize(mail->data.parts, buffer);
		index_mail_cache_add_idx(mail, cache_field,
					 buffer->data, buffer->used);
	} T_END;

	data->messageparts_saved_to_cache = TRUE;
}

static void
index_mail_body_parsed_cache_bodystructure(struct index_mail *mail,
					   enum index_cache_field field)
{
	struct mail *_mail = &mail->mail.mail;
	struct index_mail_data *data = &mail->data;
	const unsigned int cache_field_parts =
		mail->ibox->cache_fields[MAIL_CACHE_MESSAGE_PARTS].idx;
	const unsigned int cache_field_body =
		mail->ibox->cache_fields[MAIL_CACHE_IMAP_BODY].idx;
	const unsigned int cache_field_bodystructure =
		mail->ibox->cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;
	enum mail_cache_decision_type dec;
	string_t *str;
	bool bodystructure_cached = FALSE;
	bool plain_bodystructure = FALSE;
	bool cache_bodystructure, cache_body;

	if ((data->cache_flags & MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII) != 0) {
		if (data->messageparts_saved_to_cache ||
		    mail_cache_field_exists(_mail->transaction->cache_view,
					    _mail->seq, cache_field_parts) > 0) {
			/* cached it as flag + message_parts */
			plain_bodystructure = TRUE;
		}
	}

	if (!data->parsed_bodystructure)
		return;
	i_assert(data->parts != NULL);

	/* If BODY is fetched first but BODYSTRUCTURE is also wanted, we don't
	   normally want to first cache BODY and then BODYSTRUCTURE. So check
	   the wanted_fields also in here. */
	if (plain_bodystructure)
		cache_bodystructure = FALSE;
	else if (field == MAIL_CACHE_IMAP_BODYSTRUCTURE ||
		 (data->wanted_fields & MAIL_FETCH_IMAP_BODYSTRUCTURE) != 0) {
		cache_bodystructure =
			mail_cache_field_can_add(_mail->transaction->cache_trans,
				_mail->seq, cache_field_bodystructure);
	} else {
		cache_bodystructure =
			mail_cache_field_want_add(_mail->transaction->cache_trans,
				_mail->seq, cache_field_bodystructure);
	}
	if (cache_bodystructure) {
		str = str_new(mail->mail.data_pool, 128);
		imap_bodystructure_write(data->parts, str, TRUE);
		data->bodystructure = str_c(str);

		index_mail_cache_add(mail, MAIL_CACHE_IMAP_BODYSTRUCTURE,
				     str_c(str), str_len(str)+1);
		bodystructure_cached = TRUE;
	} else {
		bodystructure_cached =
			mail_cache_field_exists(_mail->transaction->cache_view,
				_mail->seq, cache_field_bodystructure) > 0;
	}

	/* normally don't cache both BODY and BODYSTRUCTURE, but do it
	   if BODY is forced to be cached */
	dec = mail_cache_field_get_decision(_mail->box->cache,
					    cache_field_body);
	if (plain_bodystructure ||
	    (bodystructure_cached &&
	     (dec != (MAIL_CACHE_DECISION_FORCED | MAIL_CACHE_DECISION_YES))))
		cache_body = FALSE;
	else if (field == MAIL_CACHE_IMAP_BODY) {
		cache_body =
			mail_cache_field_can_add(_mail->transaction->cache_trans,
				_mail->seq, cache_field_body);
	} else {
		cache_body =
			mail_cache_field_want_add(_mail->transaction->cache_trans,
				_mail->seq, cache_field_body);
	}

	if (cache_body) {
		str = str_new(mail->mail.data_pool, 128);
		imap_bodystructure_write(data->parts, str, FALSE);
		data->body = str_c(str);

		index_mail_cache_add(mail, MAIL_CACHE_IMAP_BODY,
				     str_c(str), str_len(str)+1);
	}
}

static bool
index_mail_want_cache(struct index_mail *mail, enum index_cache_field field)
{
	struct mail *_mail = &mail->mail.mail;
	enum mail_fetch_field fetch_field;
	unsigned int cache_field;

	switch (field) {
	case MAIL_CACHE_SENT_DATE:
		fetch_field = MAIL_FETCH_DATE;
		break;
	case MAIL_CACHE_RECEIVED_DATE:
		fetch_field = MAIL_FETCH_RECEIVED_DATE;
		break;
	case MAIL_CACHE_SAVE_DATE:
		fetch_field = MAIL_FETCH_SAVE_DATE;
		break;
	case MAIL_CACHE_VIRTUAL_FULL_SIZE:
		fetch_field = MAIL_FETCH_VIRTUAL_SIZE;
		break;
	case MAIL_CACHE_PHYSICAL_FULL_SIZE:
		fetch_field = MAIL_FETCH_PHYSICAL_SIZE;
		break;
	default:
		i_unreached();
	}

	if ((mail->data.dont_cache_fetch_fields & fetch_field) != 0)
		return FALSE;

	cache_field = mail->ibox->cache_fields[field].idx;
	if ((mail->data.cache_fetch_fields & fetch_field) != 0) {
		return mail_cache_field_can_add(_mail->transaction->cache_trans,
						_mail->seq, cache_field);
	} else {
		return mail_cache_field_want_add(_mail->transaction->cache_trans,
						 _mail->seq, cache_field);
	}
}

static void index_mail_cache_sizes(struct index_mail *mail)
{
	static enum index_cache_field size_fields[] = {
		MAIL_CACHE_VIRTUAL_FULL_SIZE,
		MAIL_CACHE_PHYSICAL_FULL_SIZE
	};
	uoff_t sizes[N_ELEMENTS(size_fields)];
	unsigned int i;

	sizes[0] = mail->data.virtual_size;
	sizes[1] = mail->data.physical_size;

	for (i = 0; i < N_ELEMENTS(size_fields); i++) {
		if (sizes[i] != (uoff_t)-1 &&
		    index_mail_want_cache(mail, size_fields[i])) {
			index_mail_cache_add(mail, size_fields[i],
					     &sizes[i], sizeof(sizes[i]));
		}
	}
}

static void index_mail_cache_dates(struct index_mail *mail)
{
	static enum index_cache_field date_fields[] = {
		MAIL_CACHE_RECEIVED_DATE,
		MAIL_CACHE_SAVE_DATE
	};
	time_t dates[N_ELEMENTS(date_fields)];
	unsigned int i;
	uint32_t t;

	dates[0] = mail->data.received_date;
	dates[1] = mail->mail.mail.saving ? ioloop_time :
		mail->data.save_date;

	for (i = 0; i < N_ELEMENTS(date_fields); i++) {
		if (dates[i] != (time_t)-1 &&
		    index_mail_want_cache(mail, date_fields[i])) {
			t = dates[i];
			index_mail_cache_add(mail, date_fields[i],
					     &t, sizeof(t));
		}
	}

	if (mail->data.sent_date_parsed &&
	    index_mail_want_cache(mail, MAIL_CACHE_SENT_DATE))
		(void)index_mail_cache_sent_date(mail);
}

static int
index_mail_parse_body_finish(struct index_mail *mail,
			     enum index_cache_field field, bool success)
{
	struct istream *parser_input = mail->data.parser_input;
	int ret;

	if (parser_input == NULL) {
		ret = message_parser_deinit(&mail->data.parser_ctx,
					    &mail->data.parts) < 0 ? 0 : 1;
	} else {
		mail->data.parser_input = NULL;
		i_stream_ref(parser_input);
		ret = message_parser_deinit(&mail->data.parser_ctx,
					    &mail->data.parts) < 0 ? 0 : 1;
		if (parser_input->stream_errno == 0 ||
		    parser_input->stream_errno == EPIPE) {
			/* EPIPE = input already closed. allow the caller to
			   decide if that is an error or not. (for example we
			   could be coming here from IMAP APPEND when IMAP
			   client has closed the connection too early. we
			   don't want to log an error in that case.) */
			i_assert(!success ||
				 (i_stream_read(parser_input) == -1 &&
				  !i_stream_have_bytes_left(parser_input)));
		} else {
			index_mail_stream_log_failure_for(mail, parser_input);
			ret = -1;
		}
		i_stream_unref(&parser_input);
	}
	if (ret <= 0) {
		if (ret == 0) {
			mail_set_cache_corrupted(&mail->mail.mail,
						 MAIL_FETCH_MESSAGE_PARTS);
		}
		mail->data.parts = NULL;
		mail->data.parsed_bodystructure = FALSE;
		if (mail->data.save_bodystructure_body)
			mail->data.save_bodystructure_header = TRUE;
		return -1;
	}
	if (mail->data.save_bodystructure_body) {
		mail->data.parsed_bodystructure = TRUE;
		mail->data.save_bodystructure_header = FALSE;
		mail->data.save_bodystructure_body = FALSE;
		i_assert(mail->data.parts != NULL);
	}

	if (mail->data.no_caching) {
		/* if we're here because we aborted parsing, don't get any
		   further or we may crash while generating output from
		   incomplete data */
		return 0;
	}

	(void)get_cached_msgpart_sizes(mail);

	index_mail_body_parsed_cache_flags(mail);
	index_mail_body_parsed_cache_message_parts(mail);
	index_mail_body_parsed_cache_bodystructure(mail, field);
	index_mail_cache_sizes(mail);
	index_mail_cache_dates(mail);
	return 0;
}

static void index_mail_stream_log_failure(struct index_mail *mail)
{
	index_mail_stream_log_failure_for(mail, mail->data.stream);
}

int index_mail_stream_check_failure(struct index_mail *mail)
{
	if (mail->data.stream->stream_errno == 0)
		return 0;
	index_mail_stream_log_failure(mail);
	return -1;
}

void index_mail_refresh_expunged(struct mail *mail)
{
	mail_index_refresh(mail->box->index);
	if (mail_index_is_expunged(mail->transaction->view, mail->seq))
		mail_set_expunged(mail);
}

void index_mail_stream_log_failure_for(struct index_mail *mail,
				       struct istream *input)
{
	struct mail *_mail = &mail->mail.mail;

	i_assert(input->stream_errno != 0);

	if (input->stream_errno == ENOENT) {
		/* was the mail just expunged? we could get here especially if
		   external attachments are used and the attachment is deleted
		   before we've opened the file. */
		index_mail_refresh_expunged(_mail);
		if (_mail->expunged)
			return;
	}
	mail_storage_set_critical(_mail->box->storage,
		"read(%s) failed: %s (uid=%u, box=%s)",
		i_stream_get_name(input), i_stream_get_error(input),
		_mail->uid, mailbox_get_vname(_mail->box));
}

static int index_mail_parse_body(struct index_mail *mail,
				 enum index_cache_field field)
{
	struct index_mail_data *data = &mail->data;
	uoff_t old_offset;
	int ret;

	i_assert(data->parser_ctx != NULL);

	old_offset = data->stream->v_offset;
	i_stream_seek(data->stream, data->hdr_size.physical_size);

	if (data->save_bodystructure_body) {
		/* bodystructure header is parsed, we want the body's mime
		   headers too */
		i_assert(!data->save_bodystructure_header);
		message_parser_parse_body(data->parser_ctx,
					  parse_bodystructure_part_header,
					  mail->mail.data_pool);
	} else {
		message_parser_parse_body(data->parser_ctx,
			*null_message_part_header_callback, (void *)NULL);
	}
	ret = index_mail_stream_check_failure(mail);
	if (index_mail_parse_body_finish(mail, field, TRUE) < 0)
		ret = -1;

	i_stream_seek(data->stream, old_offset);
	return ret;
}

static void index_mail_stream_destroy_callback(struct index_mail *mail)
{
	i_assert(mail->data.destroying_stream);

	mail->data.destroying_stream = FALSE;
}

void index_mail_set_read_buffer_size(struct mail *_mail, struct istream *input)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	unsigned int block_size;

	i_stream_set_max_buffer_size(input, MAIL_READ_FULL_BLOCK_SIZE);
	block_size = (mail->data.access_part & (READ_BODY | PARSE_BODY)) != 0 ?
		MAIL_READ_FULL_BLOCK_SIZE : MAIL_READ_HDR_BLOCK_SIZE;
	i_stream_set_init_buffer_size(input, block_size);
}

int index_mail_init_stream(struct index_mail *mail,
			   struct message_size *hdr_size,
			   struct message_size *body_size,
			   struct istream **stream_r)
{
	struct mail *_mail = &mail->mail.mail;
	struct index_mail_data *data = &mail->data;
	struct istream *input;
	bool has_nuls;
	int ret;

	if (!data->initialized_wrapper_stream &&
	    _mail->transaction->stats_track) {
		input = i_stream_create_mail(_mail, data->stream,
					     !data->stream_has_only_header);
		i_stream_unref(&data->stream);
		data->stream = input;
		data->initialized_wrapper_stream = TRUE;
	}

	if (!data->destroy_callback_set) {
		/* do this only once in case a plugin changes the stream.
		   otherwise the check would break. */
		data->destroy_callback_set = TRUE;
		i_stream_add_destroy_callback(data->stream,
			index_mail_stream_destroy_callback, mail);
	}

	if (hdr_size != NULL || body_size != NULL)
		(void)get_cached_msgpart_sizes(mail);

	if (hdr_size != NULL || body_size != NULL) {
		i_stream_seek(data->stream, 0);
		if (!data->hdr_size_set) {
			if ((data->access_part & PARSE_HDR) != 0) {
				(void)get_cached_parts(mail);
				if (index_mail_parse_headers(mail, NULL) < 0)
					return -1;
			} else {
				if (message_get_header_size(data->stream,
							    &data->hdr_size,
							    &has_nuls) < 0) {
					index_mail_stream_log_failure(mail);
					return -1;
				}
				data->hdr_size_set = TRUE;
			}
		}

		if (hdr_size != NULL)
			*hdr_size = data->hdr_size;
	}

	if (body_size != NULL) {
		if (!data->body_size_set)
			index_mail_get_cached_body_size(mail);
		if (!data->body_size_set) {
			i_stream_seek(data->stream,
				      data->hdr_size.physical_size);
			if ((data->access_part & PARSE_BODY) != 0) {
				if (index_mail_parse_body(mail, 0) < 0)
					return -1;
			} else {
				if (message_get_body_size(data->stream,
							  &data->body_size,
							  &has_nuls) < 0) {
					index_mail_stream_log_failure(mail);
					return -1;
				}
				data->body_size_set = TRUE;
			}
		}

		*body_size = data->body_size;
	}

	if (data->hdr_size_set && data->body_size_set) {
		data->virtual_size = data->hdr_size.virtual_size +
			data->body_size.virtual_size;
		data->physical_size = data->hdr_size.physical_size +
			data->body_size.physical_size;
	}
	ret = index_mail_stream_check_failure(mail);

	i_stream_seek(data->stream, 0);
	if (ret < 0)
		return -1;
	*stream_r = data->stream;
	return 0;
}

static int index_mail_parse_bodystructure(struct index_mail *mail,
					  enum index_cache_field field)
{
	struct index_mail_data *data = &mail->data;
	string_t *str;

	if (data->parsed_bodystructure) {
		/* we have everything parsed already, but just not written to
		   a string */
		index_mail_body_parsed_cache_bodystructure(mail, field);
	} else {
		if (data->save_bodystructure_header ||
		    !data->save_bodystructure_body) {
			/* we haven't parsed the header yet */
			data->save_bodystructure_header = TRUE;
			data->save_bodystructure_body = TRUE;
			(void)get_cached_parts(mail);
			if (index_mail_parse_headers(mail, NULL) < 0) {
				data->save_bodystructure_header = TRUE;
				return -1;
			}
			i_assert(data->parser_ctx != NULL);
		}

		if (index_mail_parse_body(mail, field) < 0)
			return -1;
	}
	i_assert(data->parts != NULL);

	/* if we didn't want to have the body(structure) cached,
	   it's still not written. */
	switch (field) {
	case MAIL_CACHE_IMAP_BODY:
		if (data->body == NULL) {
			str = str_new(mail->mail.data_pool, 128);
			imap_bodystructure_write(data->parts, str, FALSE);
			data->body = str_c(str);
		}
		break;
	case MAIL_CACHE_IMAP_BODYSTRUCTURE:
		if (data->bodystructure == NULL) {
			str = str_new(mail->mail.data_pool, 128);
			imap_bodystructure_write(data->parts, str, TRUE);
			data->bodystructure = str_c(str);
		}
		break;
	default:
		i_unreached();
	}
	return 0;
}

static void
index_mail_get_plain_bodystructure(struct index_mail *mail, string_t *str,
				   bool extended)
{
	str_printfa(str, IMAP_BODY_PLAIN_7BIT_ASCII" %"PRIuUOFF_T" %u",
		    mail->data.parts->body_size.virtual_size,
		    mail->data.parts->body_size.lines);
	if (extended)
		str_append(str, " NIL NIL NIL NIL");
}

int index_mail_get_special(struct mail *_mail,
			   enum mail_fetch_field field, const char **value_r)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	const struct mail_cache_field *cache_fields = mail->ibox->cache_fields;
	const char *error;
	string_t *str;

	switch (field) {
	case MAIL_FETCH_IMAP_BODY: {
		const unsigned int body_cache_field =
                        cache_fields[MAIL_CACHE_IMAP_BODY].idx;
		const unsigned int bodystructure_cache_field =
                        cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;

		if (data->body != NULL) {
			*value_r = data->body;
			return 0;
		}

		/* 1) use plain-7bit-ascii flag if it exists
		   2) get BODY if it exists
		   3) get it using BODYSTRUCTURE if it exists
		   4) parse body structure, and save BODY/BODYSTRUCTURE
		      depending on what we want cached */

		str = str_new(mail->mail.data_pool, 128);
		if ((mail->data.cache_flags &
		     MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII) != 0 &&
		    get_cached_parts(mail)) {
			index_mail_get_plain_bodystructure(mail, str, FALSE);
			data->body = str_c(str);
		} else if (index_mail_cache_lookup_field(mail, str,
							 body_cache_field) > 0)
			data->body = str_c(str);
		else if (index_mail_cache_lookup_field(mail, str,
					bodystructure_cache_field) > 0) {
			data->bodystructure =
				p_strdup(mail->mail.data_pool, str_c(str));
			str_truncate(str, 0);

			if (imap_body_parse_from_bodystructure(
					data->bodystructure, str, &error) < 0) {
				/* broken, continue.. */
				mail_storage_set_critical(_mail->box->storage,
					"Invalid BODYSTRUCTURE %s: %s",
					data->bodystructure, error);
				mail_set_cache_corrupted(_mail,
					MAIL_FETCH_IMAP_BODYSTRUCTURE);
			} else {
				data->body = str_c(str);
			}
		}

		if (data->body == NULL) {
			str_free(&str);
			if (index_mail_parse_bodystructure(mail,
						MAIL_CACHE_IMAP_BODY) < 0)
				return -1;
		}
		i_assert(data->body != NULL);
		*value_r = data->body;
		return 0;
	}
	case MAIL_FETCH_IMAP_BODYSTRUCTURE: {
		const unsigned int bodystructure_cache_field =
                        cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;

		if (data->bodystructure != NULL) {
			*value_r = data->bodystructure;
			return 0;
		}

		str = str_new(mail->mail.data_pool, 128);
		if ((mail->data.cache_flags &
		     MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII) != 0 &&
		    get_cached_parts(mail)) {
			index_mail_get_plain_bodystructure(mail, str, TRUE);
			data->bodystructure = str_c(str);
		} else if (index_mail_cache_lookup_field(mail, str,
					bodystructure_cache_field) > 0) {
			data->bodystructure = str_c(str);
		} else {
			str_free(&str);
			if (index_mail_parse_bodystructure(mail,
					MAIL_CACHE_IMAP_BODYSTRUCTURE) < 0)
				return -1;
		}
		i_assert(data->bodystructure != NULL);
		*value_r = data->bodystructure;
		return 0;
	}
	case MAIL_FETCH_IMAP_ENVELOPE:
		if (data->envelope == NULL) {
			if (index_mail_headers_get_envelope(mail) < 0)
				return -1;
		}
		*value_r = data->envelope;
		return 0;
	case MAIL_FETCH_FROM_ENVELOPE:
		*value_r = data->from_envelope != NULL ?
			data->from_envelope : "";
		return 0;
	case MAIL_FETCH_UIDL_FILE_NAME:
	case MAIL_FETCH_UIDL_BACKEND:
	case MAIL_FETCH_SEARCH_RELEVANCY:
	case MAIL_FETCH_GUID:
	case MAIL_FETCH_HEADER_MD5:
	case MAIL_FETCH_POP3_ORDER:
	case MAIL_FETCH_REFCOUNT:
		*value_r = "";
		return 0;
	case MAIL_FETCH_MAILBOX_NAME:
		*value_r = _mail->box->vname;
		return 0;
	default:
		i_unreached();
		return -1;
	}
}

struct mail *index_mail_get_real_mail(struct mail *mail)
{
	return mail;
}

struct mail *
index_mail_alloc(struct mailbox_transaction_context *t,
		 enum mail_fetch_field wanted_fields,
		 struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct index_mail *mail;
	pool_t pool;

	pool = pool_alloconly_create("mail", 2048);
	mail = p_new(pool, struct index_mail, 1);
	mail->mail.pool = pool;

	index_mail_init(mail, t, wanted_fields, wanted_headers);
	return &mail->mail.mail;
}

void index_mail_init(struct index_mail *mail,
		     struct mailbox_transaction_context *t,
		     enum mail_fetch_field wanted_fields,
		     struct mailbox_header_lookup_ctx *wanted_headers)
{
	array_create(&mail->mail.module_contexts, mail->mail.pool,
		     sizeof(void *), 5);

	mail->mail.v = *t->box->mail_vfuncs;
	mail->mail.mail.box = t->box;
	mail->mail.mail.transaction = t;

	t->mail_ref_count++;
	mail->mail.data_pool = pool_alloconly_create("index_mail", 16384);
	mail->ibox = INDEX_STORAGE_CONTEXT(t->box);
	mail->mail.wanted_fields = wanted_fields;
	if (wanted_headers != NULL) {
		mail->mail.wanted_headers = wanted_headers;
		mailbox_header_lookup_ref(wanted_headers);
	}
}

static void index_mail_close_streams_full(struct index_mail *mail, bool closing)
{
	struct index_mail_data *data = &mail->data;
	struct message_part *parts;

	if (data->parser_ctx != NULL) {
		if (message_parser_deinit(&data->parser_ctx, &parts) < 0) {
			mail_set_cache_corrupted(&mail->mail.mail,
						 MAIL_FETCH_MESSAGE_PARTS);
		}
		mail->data.parser_input = NULL;
		if (mail->data.save_bodystructure_body)
			mail->data.save_bodystructure_header = TRUE;
	}
	if (data->filter_stream != NULL)
		i_stream_unref(&data->filter_stream);
	if (data->stream != NULL) {
		data->destroying_stream = TRUE;
		if (!closing && data->destroy_callback_set) {
			/* we're replacing the stream with a new one. it's
			   allowed to have references until the mail is closed
			   (but we can't really check that) */
			i_stream_remove_destroy_callback(data->stream,
				index_mail_stream_destroy_callback);
		}
		i_stream_unref(&data->stream);
 		if (closing) {
			/* there must be no references to the mail when the
			   mail is being closed. */
			i_assert(!mail->data.destroying_stream);
		} else {
			data->destroying_stream = FALSE;
		}

		data->initialized_wrapper_stream = FALSE;
		data->destroy_callback_set = FALSE;
	}
}

void index_mail_close_streams(struct index_mail *mail)
{
	index_mail_close_streams_full(mail, FALSE);
}

static void index_mail_reset_data(struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;

	memset(data, 0, sizeof(*data));
	p_clear(mail->mail.data_pool);

	data->virtual_size = (uoff_t)-1;
	data->physical_size = (uoff_t)-1;
	data->save_date = (time_t)-1;
	data->received_date = (time_t)-1;
	data->sent_date.time = (uint32_t)-1;
	data->dont_cache_field_idx = UINT_MAX;

	data->wanted_fields = mail->mail.wanted_fields;
	if (mail->mail.wanted_headers != NULL) {
		data->wanted_headers = mail->mail.wanted_headers;
		mailbox_header_lookup_ref(data->wanted_headers);
	}

	mail->mail.mail.seq = 0;
	mail->mail.mail.uid = 0;
	mail->mail.seq_pvt = 0;
	mail->mail.mail.expunged = FALSE;
	mail->mail.mail.has_nuls = FALSE;
	mail->mail.mail.has_no_nuls = FALSE;
	mail->mail.mail.saving = FALSE;
}

void index_mail_close(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	/* If uid == 0 but seq != 0, we came here from saving a (non-mbox)
	   message. If that happens, don't bother checking if anything should
	   be cached since it was already checked. Also by now the transaction
	   may have already been rollbacked and seq point to a nonexistent
	   message. */
	if (mail->mail.mail.uid != 0) {
		index_mail_cache_sizes(mail);
		index_mail_cache_dates(mail);
	}

	index_mail_close_streams_full(mail, TRUE);

	if (mail->data.wanted_headers != NULL)
		mailbox_header_lookup_unref(&mail->data.wanted_headers);
	if (!mail->freeing)
		index_mail_reset_data(mail);
}

static void check_envelope(struct index_mail *mail)
{
	struct mail *_mail = &mail->mail.mail;
	const unsigned int cache_field_envelope =
		mail->ibox->cache_fields[MAIL_CACHE_IMAP_ENVELOPE].idx;
	unsigned int cache_field_hdr;

	if ((mail->data.access_part & PARSE_HDR) != 0) {
		mail->data.save_envelope = TRUE;
		return;
	}

	/* if "imap.envelope" is cached, that's all we need */
	if (mail_cache_field_exists(_mail->transaction->cache_view,
				    _mail->seq, cache_field_envelope) > 0)
		return;

	/* don't waste time doing full checks for all required
	   headers. assume that if we have "hdr.message-id" cached,
	   we don't need to parse the header. */
	cache_field_hdr = mail_cache_register_lookup(_mail->box->cache,
						     "hdr.message-id");
	if (cache_field_hdr == UINT_MAX ||
	    mail_cache_field_exists(_mail->transaction->cache_view,
				    _mail->seq, cache_field_hdr) <= 0)
		mail->data.access_part |= PARSE_HDR;
	mail->data.save_envelope = TRUE;
}

static void index_mail_update_access_parts(struct index_mail *mail)
{
	struct mail *_mail = &mail->mail.mail;
	struct index_mail_data *data = &mail->data;
	const struct mail_cache_field *cache_fields = mail->ibox->cache_fields;
	struct mail_cache_view *cache_view = _mail->transaction->cache_view;
	const struct mail_index_header *hdr;
	struct istream *input;

	if ((data->wanted_fields & (MAIL_FETCH_NUL_STATE |
				    MAIL_FETCH_IMAP_BODY |
				    MAIL_FETCH_IMAP_BODYSTRUCTURE)) != 0 &&
	    !_mail->has_nuls && !_mail->has_no_nuls) {
		(void)index_mail_get_fixed_field(mail, MAIL_CACHE_FLAGS,
						 &data->cache_flags,
						 sizeof(data->cache_flags));
		_mail->has_nuls =
			(data->cache_flags & MAIL_CACHE_FLAG_HAS_NULS) != 0;
		_mail->has_no_nuls =
			(data->cache_flags & MAIL_CACHE_FLAG_HAS_NO_NULS) != 0;
		/* we currently don't forcibly set the nul state. if it's not
		   already cached, the caller can figure out itself what to
		   do when neither is set */
	}

	/* see if wanted_fields can tell us if we need to read/parse
	   header/body */
	if ((data->wanted_fields & MAIL_FETCH_MESSAGE_PARTS) != 0 &&
	    data->parts == NULL) {
		const unsigned int cache_field =
			cache_fields[MAIL_CACHE_MESSAGE_PARTS].idx;

		if (mail_cache_field_exists(cache_view, _mail->seq,
					    cache_field) <= 0) {
			data->access_part |= PARSE_HDR | PARSE_BODY;
			data->save_message_parts = TRUE;
		}
	}

	if ((data->wanted_fields & MAIL_FETCH_IMAP_ENVELOPE) != 0 &&
	    data->envelope == NULL)
		check_envelope(mail);

	if ((data->wanted_fields & MAIL_FETCH_IMAP_BODY) != 0 &&
	    (data->cache_flags & MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII) == 0 &&
	    data->body == NULL) {
		/* we need either imap.body or imap.bodystructure */
		const unsigned int cache_field1 =
			cache_fields[MAIL_CACHE_IMAP_BODY].idx;
		const unsigned int cache_field2 =
			cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;

		if (mail_cache_field_exists(cache_view, _mail->seq,
					    cache_field1) <= 0 &&
		    mail_cache_field_exists(cache_view, _mail->seq,
					    cache_field2) <= 0) {
			data->access_part |= PARSE_HDR | PARSE_BODY;
			data->save_bodystructure_header = TRUE;
			data->save_bodystructure_body = TRUE;
		}
	}

	if ((data->wanted_fields & MAIL_FETCH_IMAP_BODYSTRUCTURE) != 0 &&
	    (data->cache_flags & MAIL_CACHE_FLAG_TEXT_PLAIN_7BIT_ASCII) == 0 &&
	    data->bodystructure == NULL) {
		const unsigned int cache_field =
			cache_fields[MAIL_CACHE_IMAP_BODYSTRUCTURE].idx;

                if (mail_cache_field_exists(cache_view, _mail->seq,
                                            cache_field) <= 0) {
			data->access_part |= PARSE_HDR | PARSE_BODY;
			data->save_bodystructure_header = TRUE;
			data->save_bodystructure_body = TRUE;
		}
	}

	if ((data->wanted_fields & MAIL_FETCH_DATE) != 0 &&
	    data->sent_date.time == (uint32_t)-1) {
		const unsigned int cache_field =
			cache_fields[MAIL_CACHE_SENT_DATE].idx;

		if (mail_cache_field_exists(cache_view, _mail->seq,
					    cache_field) <= 0) {
			data->access_part |= PARSE_HDR;
			data->save_sent_date = TRUE;
		}
	}

	if ((data->wanted_fields & (MAIL_FETCH_STREAM_HEADER |
				    MAIL_FETCH_STREAM_BODY)) != 0) {
		/* open stream immediately to set expunged flag if
		   it's already lost */
		if ((data->wanted_fields & MAIL_FETCH_STREAM_HEADER) != 0)
			data->access_part |= READ_HDR;
		if ((data->wanted_fields & MAIL_FETCH_STREAM_BODY) != 0)
			data->access_part |= READ_BODY;

		/* open the stream only if we didn't get here from
		   mailbox_save_init() */
		hdr = mail_index_get_header(_mail->transaction->view);
		if (!_mail->saving && _mail->uid < hdr->next_uid) {
			if ((data->access_part & READ_BODY) != 0)
				(void)mail_get_stream(_mail, NULL, NULL, &input);
			else
				(void)mail_get_hdr_stream(_mail, NULL, &input);
		}
	}
}

void index_mail_set_seq(struct mail *_mail, uint32_t seq, bool saving)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	if (mail->data.seq == seq) {
		if (!saving)
			return;
		/* we started saving a mail, aborted it, and now we're saving
		   another mail with the same sequence. make sure the mail
		   gets reset. */
	}

	mail->mail.v.close(&mail->mail.mail);

	mail->data.seq = seq;
	mail->mail.mail.seq = seq;
	mail->mail.mail.saving = saving;
	mail_index_lookup_uid(_mail->transaction->view, seq,
			      &mail->mail.mail.uid);

	if (mail_index_view_is_inconsistent(_mail->transaction->view)) {
		mail_set_expunged(&mail->mail.mail);
		return;
	}

	index_mail_update_access_parts(mail);
	mail->data.initialized = TRUE;
}

bool index_mail_prefetch(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
/* HAVE_POSIX_FADVISE alone isn't enough for CentOS 4.9 */
#if defined(HAVE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
	struct mail_storage *storage = _mail->box->storage;
	struct istream *input;
	off_t len;
	int fd;

	if ((storage->class_flags & MAIL_STORAGE_CLASS_FLAG_FILE_PER_MSG) == 0) {
		/* we're handling only file-per-msg storages for now. */
		return TRUE;
	}
	if (mail->data.access_part == 0) {
		/* everything we need is cached */
		return TRUE;
	}

	if (mail->data.stream == NULL) {
		(void)mail_get_stream(_mail, NULL, NULL, &input);
		if (mail->data.stream == NULL)
			return TRUE;
	}

	/* tell OS to start reading the file into memory */
	fd = i_stream_get_fd(mail->data.stream);
	if (fd != -1) {
		if ((mail->data.access_part & (READ_BODY | PARSE_BODY)) != 0)
			len = 0;
		else
			len = MAIL_READ_HDR_BLOCK_SIZE;
		if (posix_fadvise(fd, 0, len, POSIX_FADV_WILLNEED) < 0) {
			i_error("posix_fadvise(%s) failed: %m",
				i_stream_get_name(mail->data.stream));
		}
		mail->data.prefetch_sent = TRUE;
	}
#endif
	return !mail->data.prefetch_sent;
}

bool index_mail_set_uid(struct mail *_mail, uint32_t uid)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	uint32_t seq;

	if (mail_index_lookup_seq(_mail->transaction->view, uid, &seq)) {
		index_mail_set_seq(_mail, seq, FALSE);
		return TRUE;
	} else {
		mail->mail.v.close(&mail->mail.mail);
		mail->mail.mail.uid = uid;
		mail_set_expunged(&mail->mail.mail);
		return FALSE;
	}
}

void index_mail_add_temp_wanted_fields(struct mail *_mail,
				       enum mail_fetch_field fields,
				       struct mailbox_header_lookup_ctx *headers)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	ARRAY_TYPE(const_string) names;
	unsigned int i;

	data->wanted_fields |= fields;
	if (headers == NULL) {
		/* keep old ones */
	} else if (data->wanted_headers == NULL) {
		data->wanted_headers = headers;
		mailbox_header_lookup_ref(headers);
	} else {
		/* merge headers */
		t_array_init(&names, 32);
		for (i = 0; i < data->wanted_headers->count; i++)
			array_append(&names, &data->wanted_headers->name[i], 1);
		for (i = 0; i < headers->count; i++)
			array_append(&names, &headers->name[i], 1);
		array_append_zero(&names);
		if (data->wanted_headers != NULL)
			mailbox_header_lookup_unref(&data->wanted_headers);
		data->wanted_headers =
			mailbox_header_lookup_init(_mail->box,
						   array_idx(&names, 0));
	}
	index_mail_update_access_parts(mail);
}

void index_mail_set_uid_cache_updates(struct mail *_mail, bool set)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	mail->data.no_caching = set || mail->data.forced_no_caching;
}

void index_mail_free(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct mailbox_header_lookup_ctx *headers_ctx =
		(struct mailbox_header_lookup_ctx *)mail->mail.wanted_headers;

	/* make sure mailbox_search_*() users don't try to free the mail
	   directly */
	i_assert(!mail->search_mail);

	mail->freeing = TRUE;
	mail->mail.v.close(_mail);

	i_assert(_mail->transaction->mail_ref_count > 0);
	_mail->transaction->mail_ref_count--;

	if (mail->header_data != NULL)
		buffer_free(&mail->header_data);
	if (array_is_created(&mail->header_lines))
		array_free(&mail->header_lines);
	if (array_is_created(&mail->header_match))
		array_free(&mail->header_match);
	if (array_is_created(&mail->header_match_lines))
		array_free(&mail->header_match_lines);

	if (headers_ctx != NULL)
		mailbox_header_lookup_unref(&headers_ctx);
	pool_unref(&mail->mail.data_pool);
	pool_unref(&mail->mail.pool);
}

void index_mail_cache_parse_continue(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct message_block block;

	while (message_parser_parse_next_block(mail->data.parser_ctx,
					       &block) > 0) {
		if (block.size != 0)
			continue;

		if (!mail->data.header_parsed) {
			index_mail_parse_header(block.part, block.hdr, mail);
			if (block.hdr == NULL)
				mail->data.header_parsed = TRUE;
		} else {
			imap_bodystructure_parse_header(mail->mail.data_pool,
							block.part, block.hdr);
		}
	}
}

void index_mail_cache_parse_deinit(struct mail *_mail, time_t received_date,
				   bool success)
{
	struct index_mail *mail = (struct index_mail *)_mail;

	if (!success) {
		/* we're going to delete this mail anyway,
		   don't bother trying to update cache file */
		mail->data.no_caching = TRUE;
		mail->data.forced_no_caching = TRUE;

		if (mail->data.parser_ctx == NULL) {
			/* we didn't even start cache parsing */
			return;
		}
	}

	/* This is needed with 0 byte mails to get hdr=NULL call done. */
	index_mail_cache_parse_continue(_mail);

	if (mail->data.received_date == (time_t)-1)
		mail->data.received_date = received_date;
	if (mail->data.save_date == (time_t)-1) {
		/* this save_date may not be exactly the same as what we get
		   in future, but then again neither mbox nor maildir
		   guarantees it anyway. */
		mail->data.save_date = ioloop_time;
	}

	(void)index_mail_parse_body_finish(mail, 0, success);
}

static void index_mail_drop_recent_flag(struct mail *mail)
{
	const struct mail_index_header *hdr;
	uint32_t first_recent_uid = mail->uid + 1;

	hdr = mail_index_get_header(mail->transaction->view);
	if (hdr->first_recent_uid < first_recent_uid) {
		mail_index_update_header(mail->transaction->itrans,
			offsetof(struct mail_index_header, first_recent_uid),
			&first_recent_uid, sizeof(first_recent_uid), FALSE);
	}
}

static bool
index_mail_update_pvt_flags(struct mail *_mail, enum modify_type modify_type,
			    enum mail_flags pvt_flags)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	const struct mail_index_record *rec;
	enum mail_flags old_pvt_flags;

	if (!index_mail_get_pvt(_mail))
		return FALSE;
	if (pvt_flags == 0 && modify_type != MODIFY_REPLACE)
		return FALSE;

	/* see if the flags actually change anything */
	rec = mail_index_lookup(_mail->transaction->view_pvt, mail->seq_pvt);
	old_pvt_flags = rec->flags & mailbox_get_private_flags_mask(_mail->box);

	switch (modify_type) {
	case MODIFY_ADD:
		return (old_pvt_flags & pvt_flags) != pvt_flags;
	case MODIFY_REPLACE:
		return old_pvt_flags != pvt_flags;
	case MODIFY_REMOVE:
		return (old_pvt_flags & pvt_flags) != 0;
	}
	i_unreached();
}

void index_mail_update_flags(struct mail *_mail, enum modify_type modify_type,
			     enum mail_flags flags)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	enum mail_flags pvt_flags_mask, pvt_flags = 0;
	bool update_modseq = FALSE;

	if ((flags & MAIL_RECENT) == 0 &&
	    index_mailbox_is_recent(_mail->box, _mail->uid))
		index_mail_drop_recent_flag(_mail);
	flags &= MAIL_FLAGS_NONRECENT | MAIL_INDEX_MAIL_FLAG_BACKEND;

	if (_mail->box->view_pvt != NULL) {
		/* mailbox has private flags */
		pvt_flags_mask = mailbox_get_private_flags_mask(_mail->box);
		pvt_flags = flags & pvt_flags_mask;
		flags &= ~pvt_flags_mask;
		if (index_mail_update_pvt_flags(_mail, modify_type, pvt_flags)) {
			mail_index_update_flags(_mail->transaction->itrans_pvt,
						mail->seq_pvt,
						modify_type, pvt_flags);
			update_modseq = TRUE;
		}
	}

	if (!update_modseq) {
		/* no forced modseq update */
	} else if (modify_type == MODIFY_REMOVE) {
		/* add the modseq update separately */
		mail_index_update_flags(_mail->transaction->itrans, _mail->seq,
			MODIFY_ADD, (enum mail_flags )MAIL_INDEX_MAIL_FLAG_UPDATE_MODSEQ);
	} else {
		/* add as part of the flag updates */
		flags |= MAIL_INDEX_MAIL_FLAG_UPDATE_MODSEQ;
	}
	mail_index_update_flags(_mail->transaction->itrans, _mail->seq,
				modify_type, flags);
}

void index_mail_update_keywords(struct mail *mail, enum modify_type modify_type,
				struct mail_keywords *keywords)
{
	struct index_mail *imail = (struct index_mail *)mail;

	if (array_is_created(&imail->data.keyword_indexes))
		array_free(&imail->data.keyword_indexes);
	if (array_is_created(&imail->data.keywords)) {
		/* clear the keywords array so the next mail_get_keywords()
		   returns the updated keywords. don't free the array, because
		   then any existing mail_get_keywords() return values would
		   point to broken data. this won't leak memory because the
		   array is allocated from mail's memory pool. */
		memset(&imail->data.keywords, 0,
		       sizeof(imail->data.keywords));
	}

	mail_index_update_keywords(mail->transaction->itrans, mail->seq,
				   modify_type, keywords);
}

void index_mail_update_modseq(struct mail *mail, uint64_t min_modseq)
{
	mail_index_update_modseq(mail->transaction->itrans, mail->seq,
				 min_modseq);
}

void index_mail_update_pvt_modseq(struct mail *mail, uint64_t min_pvt_modseq)
{
	if (mail->box->view_pvt == NULL)
		return;
	index_transaction_init_pvt(mail->transaction);
	mail_index_update_modseq(mail->transaction->itrans_pvt, mail->seq,
				 min_pvt_modseq);
}

void index_mail_expunge(struct mail *mail)
{
	const char *value;
	guid_128_t guid_128;

	if (mail_get_special(mail, MAIL_FETCH_GUID, &value) < 0)
		mail_index_expunge(mail->transaction->itrans, mail->seq);
	else {
		mail_generate_guid_128_hash(value, guid_128);
		mail_index_expunge_guid(mail->transaction->itrans,
					mail->seq, guid_128);
	}
}

static void index_mail_parse(struct mail *mail, bool parse_body)
{
	struct index_mail *imail = (struct index_mail *)mail;

	imail->data.access_part |= PARSE_HDR;
	if (index_mail_parse_headers(imail, NULL) == 0) {
		if (parse_body) {
			imail->data.access_part |= PARSE_BODY;
			(void)index_mail_parse_body(imail, 0);
		}
	}
}

void index_mail_precache(struct mail *mail)
{
	struct index_mail *imail = (struct index_mail *)mail;
	enum mail_fetch_field cache;
	time_t date;
	uoff_t size;
	const char *str;

	if (mail_cache_field_exists_any(mail->transaction->cache_view,
					mail->seq)) {
		/* already cached this mail (we should get here only if FTS
		   plugin decreased the first precached seq) */
		return;
	}

	cache = imail->data.wanted_fields;
	if ((cache & (MAIL_FETCH_STREAM_HEADER | MAIL_FETCH_STREAM_BODY)) != 0)
		index_mail_parse(mail, (cache & MAIL_FETCH_STREAM_BODY) != 0);
	if ((cache & MAIL_FETCH_RECEIVED_DATE) != 0)
		(void)mail_get_received_date(mail, &date);
	if ((cache & MAIL_FETCH_SAVE_DATE) != 0)
		(void)mail_get_save_date(mail, &date);
	if ((cache & MAIL_FETCH_VIRTUAL_SIZE) != 0)
		(void)mail_get_virtual_size(mail, &size);
	if ((cache & MAIL_FETCH_PHYSICAL_SIZE) != 0)
		(void)mail_get_physical_size(mail, &size);
	if ((cache & MAIL_FETCH_UIDL_BACKEND) != 0)
		(void)mail_get_special(mail, MAIL_FETCH_UIDL_BACKEND, &str);
	if ((cache & MAIL_FETCH_POP3_ORDER) != 0)
		(void)mail_get_special(mail, MAIL_FETCH_POP3_ORDER, &str);
	if ((cache & MAIL_FETCH_GUID) != 0)
		(void)mail_get_special(mail, MAIL_FETCH_GUID, &str);
}

void index_mail_set_cache_corrupted(struct mail *mail,
				    enum mail_fetch_field field)
{
	struct index_mail *imail = (struct index_mail *)mail;
	const char *field_name;

	switch ((int)field) {
	case 0:
		field_name = "fields";
		break;
	case MAIL_FETCH_PHYSICAL_SIZE:
		field_name = "physical size";
		imail->data.physical_size = (uoff_t)-1;
		imail->data.virtual_size = (uoff_t)-1;
		imail->data.parts = NULL;
		break;
	case MAIL_FETCH_VIRTUAL_SIZE:
		field_name = "virtual size";
		imail->data.physical_size = (uoff_t)-1;
		imail->data.virtual_size = (uoff_t)-1;
		imail->data.parts = NULL;
		break;
	case MAIL_FETCH_MESSAGE_PARTS:
		field_name = "MIME parts";
		imail->data.parts = NULL;
		break;
	case MAIL_FETCH_IMAP_BODY:
		field_name = "IMAP BODY";
		imail->data.body = NULL;
		imail->data.bodystructure = NULL;
		break;
	case MAIL_FETCH_IMAP_BODYSTRUCTURE:
		field_name = "IMAP BODYSTRUCTURE";
		imail->data.body = NULL;
		imail->data.bodystructure = NULL;
		break;
	default:
		field_name = t_strdup_printf("#%x", field);
	}

	/* make sure we don't cache invalid values */
	mail_cache_transaction_reset(mail->transaction->cache_trans);
	imail->data.no_caching = TRUE;
	imail->data.forced_no_caching = TRUE;
	mail_cache_set_corrupted(mail->box->cache,
				 "Broken %s for mail UID %u",
				 field_name, mail->uid);
}

int index_mail_opened(struct mail *mail ATTR_UNUSED,
		      struct istream **stream ATTR_UNUSED)
{
	return 0;
}

void index_mail_save_finish(struct mail_save_context *ctx)
{
	struct index_mail *imail = (struct index_mail *)ctx->dest_mail;

	if (imail == NULL)
		return;

	if (ctx->data.from_envelope != NULL &&
	    imail->data.from_envelope == NULL) {
		imail->data.from_envelope =
			p_strdup(imail->mail.data_pool, ctx->data.from_envelope);
	}
}
