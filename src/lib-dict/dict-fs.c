/* Copyright (c) 2013-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "fs-api.h"
#include "istream.h"
#include "str.h"
#include "dict-transaction-memory.h"
#include "dict-private.h"

struct fs_dict {
	struct dict dict;
	struct fs *fs;
	char *username;
};

struct fs_dict_iterate_context {
	struct dict_iterate_context ctx;
	const char **paths;
	unsigned int path_idx;
	enum dict_iterate_flags flags;
	pool_t value_pool;
	struct fs_iter *fs_iter;
	bool failed;
};

static int
fs_dict_init(struct dict *driver, const char *uri,
	     const struct dict_settings *set,
	     struct dict **dict_r, const char **error_r)
{
	struct fs_settings fs_set;
	struct fs *fs;
	struct fs_dict *dict;
	const char *p, *fs_driver, *fs_args;

	p = strchr(uri, ':');
	if (p == NULL) {
		fs_driver = uri;
		fs_args = "";
	} else {
		fs_driver = t_strdup_until(uri, p);
		fs_args = p+1;
	}

	memset(&fs_set, 0, sizeof(fs_set));
	fs_set.username = set->username;
	fs_set.base_dir = set->base_dir;
	if (fs_init(fs_driver, fs_args, &fs_set, &fs, error_r) < 0)
		return -1;

	dict = i_new(struct fs_dict, 1);
	dict->dict = *driver;
	dict->fs = fs;
	dict->username = i_strdup(set->username);

	*dict_r = &dict->dict;
	return 0;
}

static void fs_dict_deinit(struct dict *_dict)
{
	struct fs_dict *dict = (struct fs_dict *)_dict;

	fs_deinit(&dict->fs);
	i_free(dict->username);
	i_free(dict);
}

static const char *fs_dict_get_full_key(struct fs_dict *dict, const char *key)
{
	if (strncmp(key, DICT_PATH_SHARED, strlen(DICT_PATH_SHARED)) == 0)
		return key + strlen(DICT_PATH_SHARED);
	else if (strncmp(key, DICT_PATH_PRIVATE, strlen(DICT_PATH_PRIVATE)) == 0) {
		return t_strdup_printf("%s/%s", dict->username,
				       key + strlen(DICT_PATH_PRIVATE));
	} else {
		i_unreached();
	}
}

static int fs_dict_lookup(struct dict *_dict, pool_t pool,
			  const char *key, const char **value_r)
{
	struct fs_dict *dict = (struct fs_dict *)_dict;
	struct fs_file *file;
	struct istream *input;
	const unsigned char *data;
	size_t size;
	string_t *str;
	int ret;

	file = fs_file_init(dict->fs, fs_dict_get_full_key(dict, key),
			    FS_OPEN_MODE_READONLY);
	input = fs_read_stream(file, IO_BLOCK_SIZE);
	i_stream_read(input);

	str = str_new(pool, i_stream_get_data_size(input)+1);
	while ((ret = i_stream_read_data(input, &data, &size, 0)) > 0) {
		str_append_n(str, data, size);
		i_stream_skip(input, size);
	}
	i_assert(ret == -1);

	if (input->stream_errno == 0) {
		*value_r = str_c(str);
		ret = 1;
	} else {
		*value_r = NULL;
		if (input->stream_errno == ENOENT)
			ret = 0;
	}

	i_stream_unref(&input);
	fs_file_deinit(&file);
	return ret;
}

static struct dict_iterate_context *
fs_dict_iterate_init(struct dict *_dict, const char *const *paths,
		     enum dict_iterate_flags flags)
{
	struct fs_dict *dict = (struct fs_dict *)_dict;
	struct fs_dict_iterate_context *iter;

	/* these flags are not supported for now */
	i_assert((flags & DICT_ITERATE_FLAG_RECURSE) == 0);
	i_assert((flags & (DICT_ITERATE_FLAG_SORT_BY_KEY |
			   DICT_ITERATE_FLAG_SORT_BY_VALUE)) == 0);

	iter = i_new(struct fs_dict_iterate_context, 1);
	iter->ctx.dict = _dict;
	iter->paths = p_strarray_dup(default_pool, paths);
	iter->flags = flags;
	iter->value_pool = pool_alloconly_create("iterate value pool", 128);
	iter->fs_iter = fs_iter_init(dict->fs,
				     fs_dict_get_full_key(dict, paths[0]), 0);
	return &iter->ctx;
}

static bool fs_dict_iterate(struct dict_iterate_context *ctx,
			    const char **key_r, const char **value_r)
{
	struct fs_dict_iterate_context *iter =
		(struct fs_dict_iterate_context *)ctx;
	struct fs_dict *dict = (struct fs_dict *)ctx->dict;
	const char *path;
	int ret;

	*key_r = fs_iter_next(iter->fs_iter);
	if (*key_r == NULL) {
		if (fs_iter_deinit(&iter->fs_iter) < 0) {
			iter->failed = TRUE;
			return FALSE;
		}
		if (iter->paths[++iter->path_idx] == NULL)
			return FALSE;
		path = fs_dict_get_full_key(dict, iter->paths[iter->path_idx]);
		iter->fs_iter = fs_iter_init(dict->fs, path, 0);
		return fs_dict_iterate(ctx, key_r, value_r);
	}
	if ((iter->flags & DICT_ITERATE_FLAG_NO_VALUE) != 0) {
		*value_r = NULL;
		return TRUE;
	}
	p_clear(iter->value_pool);
	path = t_strconcat(iter->paths[iter->path_idx], *key_r, NULL);
	if ((ret = fs_dict_lookup(ctx->dict, iter->value_pool, path, value_r)) < 0) {
		/* I/O error */
		iter->failed = TRUE;
		return FALSE;
	} else if (ret == 0) {
		/* file was just deleted, just skip to next one */
		return fs_dict_iterate(ctx, key_r, value_r);
	}
	return TRUE;
}

static int fs_dict_iterate_deinit(struct dict_iterate_context *ctx)
{
	struct fs_dict_iterate_context *iter =
		(struct fs_dict_iterate_context *)ctx;
	int ret;

	if (iter->fs_iter != NULL) {
		if (fs_iter_deinit(&iter->fs_iter) < 0)
			iter->failed = TRUE;
	}
	ret = iter->failed ? -1 : 0;

	pool_unref(&iter->value_pool);
	i_free(iter->paths);
	i_free(iter);
	return ret;
}

static struct dict_transaction_context *
fs_dict_transaction_init(struct dict *_dict)
{
	struct dict_transaction_memory_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create("file dict transaction", 2048);
	ctx = p_new(pool, struct dict_transaction_memory_context, 1);
	dict_transaction_memory_init(ctx, _dict, pool);
	return &ctx->ctx;
}

static int fs_dict_write_changes(struct dict_transaction_memory_context *ctx)
{
	struct fs_dict *dict = (struct fs_dict *)ctx->ctx.dict;
	struct fs_file *file;
	const struct dict_transaction_memory_change *change;
	const char *key;
	int ret = 0;

	array_foreach(&ctx->changes, change) {
		key = fs_dict_get_full_key(dict, change->key);
		switch (change->type) {
		case DICT_CHANGE_TYPE_SET:
			file = fs_file_init(dict->fs, key,
					    FS_OPEN_MODE_REPLACE);
			if (fs_write(file, change->value.str, strlen(change->value.str)) < 0) {
				i_error("fs_write(%s) failed: %s", key,
					fs_file_last_error(file));
				ret = -1;
			}
			fs_file_deinit(&file);
			break;
		case DICT_CHANGE_TYPE_UNSET:
			file = fs_file_init(dict->fs, key, FS_OPEN_MODE_READONLY);
			if (fs_delete(file) < 0) {
				i_error("fs_delete(%s) failed: %s", key,
					fs_file_last_error(file));
				ret = -1;
			}
			fs_file_deinit(&file);
			break;
		case DICT_CHANGE_TYPE_APPEND:
		case DICT_CHANGE_TYPE_INC:
			i_unreached();
		}
		if (ret < 0)
			return -1;
	}
	return 0;
}

static int
fs_dict_transaction_commit(struct dict_transaction_context *_ctx,
			   bool async ATTR_UNUSED,
			   dict_transaction_commit_callback_t *callback,
			   void *context)
{
	struct dict_transaction_memory_context *ctx =
		(struct dict_transaction_memory_context *)_ctx;
	int ret;

	if (fs_dict_write_changes(ctx) < 0)
		ret = -1;
	else
		ret = 1;
	pool_unref(&ctx->pool);

	if (callback != NULL)
		callback(ret, context);
	return ret;
}

struct dict dict_driver_fs = {
	.name = "fs",
	{
		fs_dict_init,
		fs_dict_deinit,
		NULL,
		fs_dict_lookup,
		fs_dict_iterate_init,
		fs_dict_iterate,
		fs_dict_iterate_deinit,
		fs_dict_transaction_init,
		fs_dict_transaction_commit,
		dict_transaction_memory_rollback,
		dict_transaction_memory_set,
		dict_transaction_memory_unset,
		NULL,
		NULL
	}
};
