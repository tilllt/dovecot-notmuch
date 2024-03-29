/* Copyright (c) 2007-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "abspath.h"
#include "ioloop.h"
#include "abspath.h"
#include "fs-api.h"
#include "mkdir-parents.h"
#include "unlink-old-files.h"
#include "mailbox-uidvalidity.h"
#include "mailbox-list-private.h"
#include "index-storage.h"
#include "dbox-storage.h"

#include <stdio.h>
#include <dirent.h>
#include <unistd.h>

void dbox_storage_get_list_settings(const struct mail_namespace *ns ATTR_UNUSED,
				    struct mailbox_list_settings *set)
{
	if (set->layout == NULL)
		set->layout = MAILBOX_LIST_NAME_FS;
	if (set->subscription_fname == NULL)
		set->subscription_fname = DBOX_SUBSCRIPTION_FILE_NAME;
	if (*set->maildir_name == '\0')
		set->maildir_name = DBOX_MAILDIR_NAME;
	if (*set->mailbox_dir_name == '\0')
		set->mailbox_dir_name = DBOX_MAILBOX_DIR_NAME;
}

static bool
dbox_alt_path_has_changed(const char *root_dir, const char *alt_path,
			  const char *alt_path2, const char *alt_symlink_path)
{
	const char *linkpath;

	if (t_readlink(alt_symlink_path, &linkpath) < 0) {
		if (errno == ENOENT)
			return alt_path != NULL;
		i_error("readlink(%s) failed: %m", alt_symlink_path);
		return FALSE;
	}

	if (alt_path == NULL) {
		i_warning("dbox %s: Original ALT=%s, "
			  "but currently no ALT path set", root_dir, linkpath);
		return TRUE;
	} else if (strcmp(linkpath, alt_path) != 0) {
		if (strcmp(linkpath, alt_path2) == 0) {
			/* FIXME: for backwards compatibility. old versions
			   created the symlink to mailboxes/ directory, which
			   was fine with sdbox, but didn't even exist with
			   mdbox. we'll silently replace the symlink. */
			return TRUE;
		}
		i_warning("dbox %s: Original ALT=%s, "
			  "but currently ALT=%s", root_dir, linkpath, alt_path);
		return TRUE;
	}
	return FALSE;
}

static void dbox_verify_alt_path(struct mailbox_list *list)
{
	const char *root_dir, *alt_symlink_path, *alt_path, *alt_path2;

	root_dir = mailbox_list_get_root_forced(list, MAILBOX_LIST_PATH_TYPE_DIR);
	alt_symlink_path =
		t_strconcat(root_dir, "/"DBOX_ALT_SYMLINK_NAME, NULL);
	(void)mailbox_list_get_root_path(list, MAILBOX_LIST_PATH_TYPE_ALT_DIR,
					 &alt_path);
	(void)mailbox_list_get_root_path(list, MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX,
					 &alt_path2);
	if (!dbox_alt_path_has_changed(root_dir, alt_path, alt_path2,
				       alt_symlink_path))
		return;

	/* unlink/create the current alt path symlink */
	if (unlink(alt_symlink_path) < 0 && errno != ENOENT)
		i_error("unlink(%s) failed: %m", alt_symlink_path);
	if (alt_path != NULL) {
		if (symlink(alt_path, alt_symlink_path) < 0 &&
		    errno != EEXIST) {
			i_error("symlink(%s, %s) failed: %m",
				alt_path, alt_symlink_path);
		}
	}
}

int dbox_storage_create(struct mail_storage *_storage,
			struct mail_namespace *ns,
			const char **error_r)
{
	struct dbox_storage *storage = (struct dbox_storage *)_storage;
	const struct mail_storage_settings *set = _storage->set;
	const char *error;

	if (*set->mail_attachment_fs != '\0') {
		const char *name, *args, *dir;

		args = strchr(set->mail_attachment_fs, ' ');
		if (args == NULL) {
			name = set->mail_attachment_fs;
			args = "";
		} else {
			name = t_strdup_until(set->mail_attachment_fs, args++);
		}
		if (strcmp(name, "sis-queue") == 0 &&
		    (_storage->class_flags & MAIL_STORAGE_CLASS_FLAG_FILE_PER_MSG) != 0) {
			/* FIXME: the deduplication part doesn't work, because
			   sdbox renames the files.. */
			*error_r = "mail_attachment_fs: "
				"sis-queue not currently supported by sdbox";
			return -1;
		}
		dir = mail_user_home_expand(_storage->user,
					    set->mail_attachment_dir);
		storage->attachment_dir = p_strdup(_storage->pool, dir);

		if (mailbox_list_init_fs(ns->list, name, args,
					 storage->attachment_dir,
					 &storage->attachment_fs, &error) < 0) {
			*error_r = t_strdup_printf("mail_attachment_fs: %s",
						   error);
			return -1;
		}
	}

	if (!ns->list->set.alt_dir_nocheck)
		dbox_verify_alt_path(ns->list);
	return 0;
}

void dbox_storage_destroy(struct mail_storage *_storage)
{
	struct dbox_storage *storage = (struct dbox_storage *)_storage;

	if (storage->attachment_fs != NULL)
		fs_deinit(&storage->attachment_fs);
	index_storage_destroy(_storage);
}

uint32_t dbox_get_uidvalidity_next(struct mailbox_list *list)
{
	const char *path;

	path = mailbox_list_get_root_forced(list, MAILBOX_LIST_PATH_TYPE_CONTROL);
	path = t_strconcat(path, "/"DBOX_UIDVALIDITY_FILE_NAME, NULL);
	return mailbox_uidvalidity_next(list, path);
}

void dbox_notify_changes(struct mailbox *box)
{
	const char *dir, *path;

	if (box->notify_callback == NULL)
		index_mailbox_check_remove_all(box);
	else {
		if (mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_INDEX,
					&dir) <= 0)
			return;
		path = t_strdup_printf("%s/"MAIL_INDEX_PREFIX".log", dir);
		index_mailbox_check_add(box, path);
	}
}

static bool
dbox_cleanup_if_exists(struct mailbox_list *list, const char *path)
{
	struct stat st;
	unsigned int interval = list->mail_set->mail_temp_scan_interval;

	if (stat(path, &st) < 0)
		return FALSE;

	/* check once in a while if there are temp files to clean up */
	if (interval == 0) {
		/* disabled */
	} else if (st.st_atime > st.st_ctime + DBOX_TMP_DELETE_SECS) {
		/* there haven't been any changes to this directory since we
		   last checked it. */
	} else if (st.st_atime < ioloop_time - (time_t)interval) {
		/* time to scan */
		const char *prefix =
			mailbox_list_get_global_temp_prefix(list);

		(void)unlink_old_files(path, prefix,
				       ioloop_time - DBOX_TMP_DELETE_SECS);
	}
	return TRUE;
}

int dbox_mailbox_open(struct mailbox *box)
{
	const char *box_path = mailbox_get_path(box);

	if (dbox_cleanup_if_exists(box->list, box_path))
		;
	else if (errno == ENOENT || errno == ENAMETOOLONG) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTFOUND,
			T_MAIL_ERR_MAILBOX_NOT_FOUND(box->vname));
		return -1;
	} else if (errno == EACCES) {
		mail_storage_set_critical(box->storage, "%s",
			mail_error_eacces_msg("stat", box_path));
		return -1;
	} else {
		mail_storage_set_critical(box->storage,
					  "stat(%s) failed: %m", box_path);
		return -1;
	}

	if (index_storage_mailbox_open(box, FALSE) < 0)
		return -1;
	mail_index_set_fsync_mode(box->index,
				  box->storage->set->parsed_fsync_mode,
				  MAIL_INDEX_FSYNC_MASK_APPENDS |
				  MAIL_INDEX_FSYNC_MASK_EXPUNGES);
	return 0;
}

static int dir_is_empty(struct mail_storage *storage, const char *path)
{
	DIR *dir;
	struct dirent *d;
	int ret = 1;

	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT) {
			/* race condition with DELETE/RENAME? */
			return 1;
		}
		mail_storage_set_critical(storage, "opendir(%s) failed: %m",
					  path);
		return -1;
	}
	while ((d = readdir(dir)) != NULL) {
		if (*d->d_name == '.')
			continue;

		ret = 0;
		break;
	}
	if (closedir(dir) < 0) {
		mail_storage_set_critical(storage, "closedir(%s) failed: %m",
					  path);
		ret = -1;
	}
	return ret;
}

int dbox_mailbox_create(struct mailbox *box,
			const struct mailbox_update *update, bool directory)
{
	struct dbox_storage *storage = (struct dbox_storage *)box->storage;
	const char *alt_path;
	struct stat st;
	int ret;

	if ((ret = index_storage_mailbox_create(box, directory)) <= 0)
		return ret;
	if (mailbox_open(box) < 0)
		return -1;

	if (mail_index_get_header(box->view)->uid_validity != 0) {
		mail_storage_set_error(box->storage, MAIL_ERROR_EXISTS,
				       "Mailbox already exists");
		return -1;
	}

	/* if alt path already exists and contains files, rebuild storage so
	   that we don't start overwriting files. */
	ret = mailbox_get_path_to(box, MAILBOX_LIST_PATH_TYPE_ALT_MAILBOX, &alt_path);
	if (ret > 0 && stat(alt_path, &st) == 0) {
		ret = dir_is_empty(box->storage, alt_path);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			mail_storage_set_critical(&storage->storage,
				"Mailbox %s has existing files in alt path, "
				"rebuilding storage to avoid losing messages",
				box->vname);
			storage->v.set_mailbox_corrupted(box);
			return -1;
		}
		/* dir is empty, ignore it */
	}
	return dbox_mailbox_create_indexes(box, update);
}

int dbox_mailbox_create_indexes(struct mailbox *box,
				const struct mailbox_update *update)
{
	struct dbox_storage *storage = (struct dbox_storage *)box->storage;
	struct mail_index_sync_ctx *sync_ctx;
	struct mail_index_view *view;
	struct mail_index_transaction *trans;
	int ret;

	/* use syncing as a lock */
	ret = mail_index_sync_begin(box->index, &sync_ctx, &view, &trans, 0);
	if (ret <= 0) {
		i_assert(ret != 0);
		mailbox_set_index_error(box);
		return -1;
	}

	if (mail_index_get_header(view)->uid_validity == 0) {
		if (storage->v.mailbox_create_indexes(box, update, trans) < 0) {
			mail_index_sync_rollback(&sync_ctx);
			return -1;
		}
	}

	return mail_index_sync_commit(&sync_ctx);
}

int dbox_verify_alt_storage(struct mailbox_list *list)
{
	const char *alt_path;
	struct stat st;

	if (!mailbox_list_get_root_path(list, MAILBOX_LIST_PATH_TYPE_ALT_DIR,
					&alt_path))
		return 0;

	/* make sure alt storage is mounted. if it's not, abort the rebuild. */
	if (stat(alt_path, &st) == 0)
		return 0;
	if (errno != ENOENT) {
		i_error("stat(%s) failed: %m", alt_path);
		return -1;
	}

	/* try to create the alt directory. if it fails, it means alt
	   storage isn't mounted. */
	if (mailbox_list_mkdir_root(list, alt_path,
				    MAILBOX_LIST_PATH_TYPE_ALT_DIR) < 0)
		return -1;
	return 0;
}

bool dbox_header_have_flag(struct mailbox *box, uint32_t ext_id,
			   unsigned int flags_offset, uint8_t flag)
{
	const void *data;
	size_t data_size;
	uint8_t flags = 0;

	mail_index_get_header_ext(box->view, ext_id, &data, &data_size);
	if (flags_offset < data_size)
		flags = *((const uint8_t *)data + flags_offset);
	return (flags & flag) != 0;
}
