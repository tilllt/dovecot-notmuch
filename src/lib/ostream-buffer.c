/* Copyright (c) 2002-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "ostream-private.h"

struct buffer_ostream {
	struct ostream_private ostream;
	buffer_t *buf;
	bool seeked;
};

static int o_stream_buffer_seek(struct ostream_private *stream, uoff_t offset)
{
	struct buffer_ostream *bstream = (struct buffer_ostream *)stream;

	bstream->seeked = TRUE;
	stream->ostream.offset = offset;
	return 1;
}

static int
o_stream_buffer_write_at(struct ostream_private *stream,
			 const void *data, size_t size, uoff_t offset)
{
	struct buffer_ostream *bstream = (struct buffer_ostream *)stream;

	buffer_write(bstream->buf, offset, data, size);
	return 0;
}

static ssize_t
o_stream_buffer_sendv(struct ostream_private *stream,
		      const struct const_iovec *iov, unsigned int iov_count)
{
	struct buffer_ostream *bstream = (struct buffer_ostream *)stream;
	size_t left, n, offset;
	ssize_t ret = 0;
	unsigned int i;

	offset = bstream->seeked ? stream->ostream.offset : bstream->buf->used;

	for (i = 0; i < iov_count; i++) {
		left = bstream->ostream.max_buffer_size -
			stream->ostream.offset;
		n = I_MIN(left, iov[i].iov_len);
		buffer_write(bstream->buf, offset, iov[i].iov_base, n);
		stream->ostream.offset += n; offset += n;
		ret += n;
		if (n != iov[i].iov_len)
			break;
	}
	return ret;
}

struct ostream *o_stream_create_buffer(buffer_t *buf)
{
	struct buffer_ostream *bstream;
	struct ostream *output;

	bstream = i_new(struct buffer_ostream, 1);
	bstream->ostream.max_buffer_size = (size_t)-1;
	bstream->ostream.seek = o_stream_buffer_seek;
	bstream->ostream.sendv = o_stream_buffer_sendv;
	bstream->ostream.write_at = o_stream_buffer_write_at;

	bstream->buf = buf;
	output = o_stream_create(&bstream->ostream, NULL, -1);
	o_stream_set_name(output, "(buffer)");
	return output;
}
