#ifndef ISTREAM_H
#define ISTREAM_H

/* Note that some systems (Solaris) may use a macro to redefine struct stat */
#include <sys/stat.h>

struct istream {
	uoff_t v_offset;

	int stream_errno;
	unsigned int mmaped:1; /* be careful when copying data */
	unsigned int blocking:1; /* read() shouldn't return 0 */
	unsigned int closed:1;
	unsigned int readable_fd:1; /* fd can be read directly if necessary
	                               (for sendfile()) */
	unsigned int seekable:1; /* we can seek() backwards */
	unsigned int eof:1; /* read() has reached to end of file
	                       (but may still be data available in buffer) */

	struct istream_private *real_stream;
};

typedef void istream_callback_t(void *context);

struct istream *i_stream_create_fd(int fd, size_t max_buffer_size,
				   bool autoclose_fd);
/* The fd is set to -1 immediately to avoid accidentally closing it twice. */
struct istream *i_stream_create_fd_autoclose(int *fd, size_t max_buffer_size);
/* Open the given path only when something is actually tried to be read from
   the stream. */
struct istream *i_stream_create_file(const char *path, size_t max_buffer_size);
struct istream *i_stream_create_mmap(int fd, size_t block_size,
				     uoff_t start_offset, uoff_t v_size,
				     bool autoclose_fd);
struct istream *i_stream_create_from_data(const void *data, size_t size);
struct istream *i_stream_create_limit(struct istream *input, uoff_t v_size);
struct istream *i_stream_create_range(struct istream *input,
				      uoff_t v_offset, uoff_t v_size);
struct istream *i_stream_create_error(int stream_errno);
struct istream *
i_stream_create_error_str(int stream_errno, const char *fmt, ...)
	ATTR_FORMAT(2, 3);

/* Set name (e.g. path) for input stream. */
void i_stream_set_name(struct istream *stream, const char *name);
/* Get input stream's name. If stream itself doesn't have a name,
   it looks up further into stream's parents until one of them has a name.
   Returns "" if stream has no name. */
const char *i_stream_get_name(struct istream *stream);

/* Close this stream (but not its parents) and unreference it. */
void i_stream_destroy(struct istream **stream);

/* Reference counting. References start from 1, so calling i_stream_unref()
   destroys the stream if i_stream_ref() is never used. */
void i_stream_ref(struct istream *stream);
/* Unreferences the stream and sets stream pointer to NULL. */
void i_stream_unref(struct istream **stream);
/* Call the given callback function when stream is destroyed. */
void i_stream_add_destroy_callback(struct istream *stream,
				   istream_callback_t *callback, void *context)
	ATTR_NULL(3);
#define i_stream_add_destroy_callback(stream, callback, context) \
	i_stream_add_destroy_callback(stream + \
		CALLBACK_TYPECHECK(callback, void (*)(typeof(context))), \
		(istream_callback_t *)callback, context)
/* Remove the destroy callback. */
void i_stream_remove_destroy_callback(struct istream *stream,
				      void (*callback)());

/* Return file descriptor for stream, or -1 if none is available. */
int i_stream_get_fd(struct istream *stream);
/* Returns error string for the last error. */
const char *i_stream_get_error(struct istream *stream);

/* Mark the stream and all of its parent streams closed. Any reads after this
   will return -1. The data already read can still be used. */
void i_stream_close(struct istream *stream);
/* Sync the stream with the underlying backend, ie. if a file has been
   modified, flush any cached data. */
void i_stream_sync(struct istream *stream);

/* Change the initial size for stream's input buffer. This basically just
   grows the read buffer size from the default. This function has no effect
   unless it's called before reading anything. */
void i_stream_set_init_buffer_size(struct istream *stream, size_t size);
/* Change the maximum size for stream's input buffer to grow. Useful only
   for buffered streams (currently only file). */
void i_stream_set_max_buffer_size(struct istream *stream, size_t max_size);
/* Returns the current max. buffer size. */
size_t i_stream_get_max_buffer_size(struct istream *stream);
/* Enable/disable i_stream[_read]_next_line() returning the last line if it
   doesn't end with LF. */
void i_stream_set_return_partial_line(struct istream *stream, bool set);

/* Returns number of bytes read if read was ok, -1 if EOF or error, -2 if the
   input buffer is full. */
ssize_t i_stream_read(struct istream *stream);
/* Skip forward a number of bytes. Never fails, the next read tells if it
   was successful. */
void i_stream_skip(struct istream *stream, uoff_t count);
/* Seek to specified position from beginning of file. Never fails, the next
   read tells if it was successful. This works only for files. */
void i_stream_seek(struct istream *stream, uoff_t v_offset);
/* Like i_stream_seek(), but also giving a hint that after reading some data
   we could be seeking back to this mark or somewhere after it. If input
   stream's implementation is slow in seeking backwards, it can use this hint
   to cache some of the data in memory. */
void i_stream_seek_mark(struct istream *stream, uoff_t v_offset);
/* Returns 0 if ok, -1 if error. As the underlying stream may not be
   a file, only some of the fields might be set, others would be zero.
   st_size is always set, and if it's not known, it's -1.

   If exact=FALSE, the stream may not return exactly correct values, but the
   returned values can be compared to see if anything had changed (eg. in
   compressed stream st_size could be compressed size) */
int i_stream_stat(struct istream *stream, bool exact, const struct stat **st_r);
/* Similar to i_stream_stat() call. Returns 1 if size was successfully
   set, 0 if size is unknown, -1 if error. */
int i_stream_get_size(struct istream *stream, bool exact, uoff_t *size_r);
/* Returns TRUE if there are any bytes left to be read or in buffer. */
bool i_stream_have_bytes_left(struct istream *stream);
/* Returns TRUE if there are no bytes buffered and read() returns EOF. */
bool i_stream_is_eof(struct istream *stream);
/* Returns the absolute offset of the stream. This is the stream's current
   v_offset + the parent's absolute offset when the stream was created. */
uoff_t i_stream_get_absolute_offset(struct istream *stream);

/* Gets the next line from stream and returns it, or NULL if more data is
   needed to make a full line. i_stream_set_return_partial_line() specifies
   if the last line should be returned if it doesn't end with LF. */
char *i_stream_next_line(struct istream *stream);
/* Like i_stream_next_line(), but reads for more data if needed. Returns NULL
   if more data is needed or error occurred. If the input buffer gets full,
   stream_errno is set to ENOBUFS. */
char *i_stream_read_next_line(struct istream *stream);
/* Returns TRUE if the last line read with i_stream_next_line() ended with
   CRLF (instead of LF). */
bool i_stream_last_line_crlf(struct istream *stream);

/* Returns pointer to beginning of read data, or NULL if there's no data
   buffered. */
const unsigned char *i_stream_get_data(struct istream *stream, size_t *size_r);
size_t i_stream_get_data_size(struct istream *stream);
/* Like i_stream_get_data(), but returns non-const data. This only works with
   buffered streams (currently only file), others return NULL. */
unsigned char *i_stream_get_modifiable_data(struct istream *stream,
					    size_t *size_r);
/* Like i_stream_get_data(), but read more when needed. Returns 1 if more
   than threshold bytes are available, 0 if as much or less, -1 if error or
   EOF with no bytes read that weren't already in buffer, or -2 if stream's
   input buffer is full. */
int i_stream_read_data(struct istream *stream, const unsigned char **data_r,
		       size_t *size_r, size_t threshold);

/* Append external data to input stream. Returns TRUE if successful, FALSE if
   there is not enough space in the stream. */
bool i_stream_add_data(struct istream *stream, const unsigned char *data,
		       size_t size);

void i_stream_set_input_pending(struct istream *stream, bool pending);

/* If there are any I/O loop items associated with the stream, move all of
   them to current_ioloop. */
void i_stream_switch_ioloop(struct istream *stream);

#endif
