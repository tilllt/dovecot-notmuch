/* Copyright (c) 2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "str.h"
#include "safe-mkstemp.h"
#include "write-full.h"
#include "istream.h"
#include "ostream-private.h"
#include "iostream-temp.h"

#include <unistd.h>

#define IOSTREAM_TEMP_MAX_BUF_SIZE (1024*128)

struct temp_ostream {
	struct ostream_private ostream;
	char *temp_path_prefix;
	buffer_t *buf;
	int fd;
	bool fd_tried;
};

static void o_stream_temp_close(struct iostream_private *stream)
{
	struct temp_ostream *tstream = (struct temp_ostream *)stream;

	if (tstream->fd != -1)
		i_close_fd(&tstream->fd);
	if (tstream->buf != NULL)
		buffer_free(&tstream->buf);
	i_free(tstream->temp_path_prefix);
}

static int o_stream_temp_move_to_fd(struct temp_ostream *tstream)
{
	string_t *path;

	if (tstream->fd_tried)
		return -1;
	tstream->fd_tried = TRUE;

	path = t_str_new(128);
	str_append(path, tstream->temp_path_prefix);
	tstream->fd = safe_mkstemp_hostpid(path, 0600, (uid_t)-1, (gid_t)-1);
	if (tstream->fd == -1) {
		i_error("safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}
	if (unlink(str_c(path)) < 0) {
		i_error("unlink(%s) failed: %m", str_c(path));
		i_close_fd(&tstream->fd);
		return -1;
	}
	if (write_full(tstream->fd, tstream->buf->data, tstream->buf->used) < 0) {
		i_error("write(%s) failed: %m", str_c(path));
		i_close_fd(&tstream->fd);
		return -1;
	}
	buffer_free(&tstream->buf);
	return 0;
}

static ssize_t
o_stream_temp_fd_sendv(struct temp_ostream *tstream,
		       const struct const_iovec *iov, unsigned int iov_count)
{
	size_t bytes = 0;
	unsigned int i;

	for (i = 0; i < iov_count; i++) {
		if (write_full(tstream->fd, iov[i].iov_base, iov[i].iov_len) < 0) {
			tstream->ostream.ostream.stream_errno = errno;
			return -1;
		}
		bytes += iov[i].iov_len;
		tstream->ostream.ostream.offset += iov[i].iov_len;
	}
	return bytes;
}

static ssize_t
o_stream_temp_sendv(struct ostream_private *stream,
		    const struct const_iovec *iov, unsigned int iov_count)
{
	struct temp_ostream *tstream = (struct temp_ostream *)stream;
	ssize_t ret = 0;
	unsigned int i;

	if (tstream->fd != -1)
		return o_stream_temp_fd_sendv(tstream, iov, iov_count);

	for (i = 0; i < iov_count; i++) {
		if (tstream->buf->used + iov[i].iov_len > IOSTREAM_TEMP_MAX_BUF_SIZE) {
			if (o_stream_temp_move_to_fd(tstream) == 0) {
				return o_stream_temp_fd_sendv(tstream, iov+i,
							      iov_count-i);
			}
			/* failed to move to temp fd, just keep it in memory */
		}
		buffer_append(tstream->buf, iov[i].iov_base, iov[i].iov_len);
		ret += iov[i].iov_len;
		stream->ostream.offset += iov[i].iov_len;
	}
	return ret;
}

struct ostream *iostream_temp_create(const char *temp_path_prefix)
{
	struct temp_ostream *tstream;
	struct ostream *output;

	tstream = i_new(struct temp_ostream, 1);
	tstream->ostream.sendv = o_stream_temp_sendv;
	tstream->ostream.iostream.close = o_stream_temp_close;
	tstream->temp_path_prefix = i_strdup(temp_path_prefix);
	tstream->buf = buffer_create_dynamic(default_pool, 8192);
	tstream->fd = -1;

	output = o_stream_create(&tstream->ostream, NULL, -1);
	o_stream_set_name(output, "(temp iostream)");
	return output;
}

static void iostream_temp_buf_destroyed(buffer_t *buf)
{
	buffer_free(&buf);
}

struct istream *iostream_temp_finish(struct ostream **output,
				     size_t max_buffer_size)
{
	struct temp_ostream *tstream =
		(struct temp_ostream *)(*output)->real_stream;
	struct istream *input;

	if (tstream->fd != -1) {
		input = i_stream_create_fd(tstream->fd, max_buffer_size, TRUE);
		tstream->fd = -1;
	} else {
		input = i_stream_create_from_data(tstream->buf->data,
						  tstream->buf->used);
		i_stream_set_destroy_callback(input, iostream_temp_buf_destroyed,
					      tstream->buf);
		tstream->buf = NULL;
	}
	o_stream_destroy(output);
	return input;
}
