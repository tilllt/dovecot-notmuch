/* Copyright (c) 2013-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "llist.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "istream-timeout.h"
#include "ostream.h"
#include "time-util.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "http-response-parser.h"

#include "http-client-private.h"

/*
 * Logging
 */

static inline void
http_client_connection_debug(struct http_client_connection *conn,
	const char *format, ...) ATTR_FORMAT(2, 3);

static inline void
http_client_connection_debug(struct http_client_connection *conn,
	const char *format, ...)
{
	va_list args;

	if (conn->client->set.debug) {

		va_start(args, format);	
		i_debug("http-client: conn %s: %s",
			http_client_connection_label(conn),	t_strdup_vprintf(format, args));
		va_end(args);
	}
}

/*
 * Connection
 */

static void http_client_connection_input(struct connection *_conn);
static void
http_client_connection_disconnect(struct http_client_connection *conn);

unsigned int
http_client_connection_count_pending(struct http_client_connection *conn)
{
	unsigned int pending_count = array_count(&conn->request_wait_list);

	if (conn->in_req_callback || conn->pending_request != NULL)
		pending_count++;
	return pending_count;
}

bool http_client_connection_is_idle(struct http_client_connection *conn)
{
	return (conn->to_idle != NULL);
}

static void
http_client_connection_retry_requests(struct http_client_connection *conn,
	unsigned int status, const char *error)
{
	struct http_client_request **req;

	if (!array_is_created(&conn->request_wait_list))
		return;

	array_foreach_modifiable(&conn->request_wait_list, req) {
		if ((*req)->state < HTTP_REQUEST_STATE_FINISHED)
			http_client_request_retry(*req, status, error);
		http_client_request_unref(req);
	}	
	array_clear(&conn->request_wait_list);
}

static void
http_client_connection_server_close(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_request **req;

	http_client_connection_debug(conn,
		"Server explicitly closed connection");

	http_client_connection_disconnect(conn);

	array_foreach_modifiable(&conn->request_wait_list, req) {
		if ((*req)->state < HTTP_REQUEST_STATE_FINISHED)
			http_client_request_resubmit(*req);
		http_client_request_unref(req);
	}	
	array_clear(&conn->request_wait_list);

	if (conn->client->ioloop != NULL)
		io_loop_stop(conn->client->ioloop);

	http_client_connection_unref(_conn);
}

static void
http_client_connection_abort_error(struct http_client_connection **_conn,
	unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_request **req;

	http_client_connection_debug(conn, "Aborting connection: %s", error);

	http_client_connection_disconnect(conn);

	array_foreach_modifiable(&conn->request_wait_list, req) {
		i_assert((*req)->submitted);
		http_client_request_error(*req, status, error);
		http_client_request_unref(req);
	}
	array_clear(&conn->request_wait_list);
	http_client_connection_unref(_conn);
}

static void
http_client_connection_abort_temp_error(struct http_client_connection **_conn,
	unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;
	const char *sslerr;

	if (status == HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST &&
	    conn->ssl_iostream != NULL) {
		sslerr = ssl_iostream_get_last_error(conn->ssl_iostream);
		if (sslerr != NULL) {
			error = t_strdup_printf("%s (last SSL error: %s)",
						error, sslerr);
		}
		if (ssl_iostream_has_handshake_failed(conn->ssl_iostream)) {
			/* this isn't really a "connection lost", but that we
			   don't trust the remote's SSL certificate. don't
			   retry. */
			http_client_connection_abort_error(_conn,
				HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE, error);
			return;
		}
	}

	http_client_connection_debug(conn,
		"Aborting connection with temporary error: %s", error);

	http_client_connection_disconnect(conn);
	
	http_client_connection_retry_requests(conn, status, error);
	http_client_connection_unref(_conn);
}

bool http_client_connection_is_ready(struct http_client_connection *conn)
{
	int ret;

	if (conn->in_req_callback) {
		/* this can happen when a nested ioloop is created inside request
		   callback. we currently don't reuse connections that are occupied
		   this way, but theoretically we could, although that would add
		   quite a bit of complexity.
		 */
		return FALSE;
	}

	if (!conn->connected || conn->output_locked ||
		conn->close_indicated || conn->tunneling ||
		http_client_connection_count_pending(conn) >=
			conn->client->set.max_pipelined_requests)
		return FALSE;

	if (conn->last_ioloop != NULL && conn->last_ioloop != current_ioloop) {
		conn->last_ioloop = current_ioloop;
		/* Active ioloop is different from what we saw earlier;
		   we may have missed a disconnection event on this connection.
		   Verify status by reading from connection. */
		if ((ret=i_stream_read(conn->conn.input)) < 0) {
			int stream_errno = conn->conn.input->stream_errno;

			i_assert(ret != -2);
			i_assert(conn->conn.input->stream_errno != 0 || conn->conn.input->eof);
			http_client_connection_abort_temp_error(&conn,
				HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
				t_strdup_printf("Connection lost: read(%s) failed: %s",
						i_stream_get_name(conn->conn.input),
						stream_errno != 0 ?
						i_stream_get_error(conn->conn.input) :
						"EOF"));
			return FALSE;
		}
	}
	return TRUE;
}

static void
http_client_connection_idle_timeout(struct http_client_connection *conn)
{
	http_client_connection_debug(conn, "Idle connection timed out");

	/* cannot get here unless connection was established at some point */
	i_assert(conn->connect_succeeded);

	http_client_connection_disconnect(conn);
	http_client_connection_unref(&conn);
}

void http_client_connection_check_idle(struct http_client_connection *conn)
{
	unsigned int timeout, count;

	if (conn->connected &&
		array_is_created(&conn->request_wait_list) &&
		array_count(&conn->request_wait_list) == 0 &&
		!conn->in_req_callback &&
		conn->incoming_payload == NULL &&
		conn->client->set.max_idle_time_msecs > 0) {

		if (conn->to_idle != NULL) {
			/* timeout already set */
			return;
		}

		if (conn->client->ioloop != NULL)
			io_loop_stop(conn->client->ioloop);

		count = array_count(&conn->peer->conns);
		i_assert(count > 0);

		/* set timeout for this connection */
		if (count > conn->client->set.max_parallel_connections) {
			/* instant death for (urgent) connections above limit */
			timeout = 0;
		} else {
			unsigned int idle_count = http_client_peer_idle_connections(conn->peer);

			/* kill duplicate connections quicker;
				 linearly based on the number of connections */
			i_assert(count >= idle_count + 1);
			timeout = (conn->client->set.max_parallel_connections - idle_count) *
				(conn->client->set.max_idle_time_msecs /
					conn->client->set.max_parallel_connections);
		}

		http_client_connection_debug(conn, 
			"No more requests queued; going idle (timeout = %u msecs)",
			timeout);

		conn->to_idle =
			timeout_add(timeout, http_client_connection_idle_timeout, conn);

	} else {
		/* there should be no idle timeout */
		i_assert(conn->to_idle == NULL);
	}
}

static void
http_client_connection_request_timeout(struct http_client_connection *conn)
{
	unsigned int msecs = conn->client->set.request_timeout_msecs;

	conn->conn.input->stream_errno = ETIMEDOUT;
	http_client_connection_abort_temp_error(&conn,
		HTTP_CLIENT_REQUEST_ERROR_TIMED_OUT, t_strdup_printf(
		"No response for request in %u.%03u secs",
		msecs/1000, msecs%1000));
}

static void
http_client_connection_continue_timeout(struct http_client_connection *conn)
{
	struct http_client_request *const *req_idx;
	struct http_client_request *req;
	const char *error;

	if (conn->to_response != NULL)
		timeout_remove(&conn->to_response);
	conn->peer->no_payload_sync = TRUE;

	http_client_connection_debug(conn, 
		"Expected 100-continue response timed out; sending payload anyway");

	i_assert(array_count(&conn->request_wait_list) > 0);
	req_idx = array_idx(&conn->request_wait_list,
		array_count(&conn->request_wait_list)-1);
	req = req_idx[0];

	conn->payload_continue = TRUE;
	if (http_client_request_send_more(req, &error) < 0) {
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Failed to send request: %s", error));
	}
}

int http_client_connection_next_request(struct http_client_connection *conn)
{
	struct http_client_request *req = NULL;
	const char *error;
	bool have_pending_requests;

	if (!http_client_connection_is_ready(conn)) {
		http_client_connection_debug(conn, "Not ready for next request");
		return 0;
	}

	/* claim request, but no urgent request can be second in line */
	have_pending_requests = array_count(&conn->request_wait_list) > 0 ||
		conn->pending_request != NULL;
	req = http_client_peer_claim_request(conn->peer, have_pending_requests);
	if (req == NULL)
		return 0;	

	if (conn->to_idle != NULL)
		timeout_remove(&conn->to_idle);

	if (conn->client->set.request_timeout_msecs == 0)
		;
	else if (conn->to_requests != NULL)
		timeout_reset(conn->to_requests);
	else {
		conn->to_requests = timeout_add(conn->client->set.request_timeout_msecs,
						http_client_connection_request_timeout, conn);
	}
	req->conn = conn;
	conn->payload_continue = FALSE;
	if (conn->peer->no_payload_sync)
		req->payload_sync = FALSE;

	i_assert(req->state == HTTP_REQUEST_STATE_QUEUED);
	array_append(&conn->request_wait_list, &req, 1);
	http_client_request_ref(req);

	http_client_connection_debug(conn, "Claimed request %s",
		http_client_request_label(req));

	if (http_client_request_send(req, &error) < 0) {
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Failed to send request: %s", error));
		return -1;
	}

	if (req->connect_tunnel)
		conn->tunneling = TRUE;

	/* RFC 7231, Section 5.1.1: Expect

		 o  A client that sends a 100-continue expectation is not required to
		    wait for any specific length of time; such a client MAY proceed to
		    send the message body even if it has not yet received a response.
		    Furthermore, since 100 (Continue) responses cannot be sent through
		    an HTTP/1.0 intermediary, such a client SHOULD NOT wait for an
		    indefinite period before sending the message body.
	 */
	if (req->payload_sync && !conn->peer->seen_100_response) {
		i_assert(req->payload_chunked || req->payload_size > 0);
		i_assert(conn->to_response == NULL);
		conn->to_response =	timeout_add(HTTP_CLIENT_CONTINUE_TIMEOUT_MSECS,
			http_client_connection_continue_timeout, conn);
	}

	return 1;
}

static void http_client_connection_destroy(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	const char *error;
	unsigned int msecs;

	conn->closing = TRUE;
	conn->connected = FALSE;

	switch (_conn->disconnect_reason) {
	case CONNECTION_DISCONNECT_CONNECT_TIMEOUT:
		if (conn->connected_timestamp.tv_sec == 0) {
			msecs = timeval_diff_msecs(&ioloop_timeval,
						   &conn->connect_start_timestamp);
			error = t_strdup_printf(
				"connect(%s) failed: Connection timed out in %u.%03u secs",
				_conn->name, msecs/1000, msecs%1000);
		} else {
			msecs = timeval_diff_msecs(&ioloop_timeval,
						   &conn->connected_timestamp);
			error = t_strdup_printf(
				"SSL handshaking to %s failed: Connection timed out in %u.%03u secs",
				_conn->name, msecs/1000, msecs%1000);
		}
		http_client_connection_debug(conn, "%s", error);
		http_client_peer_connection_failure(conn->peer, error);
		break;
	case CONNECTION_DISCONNECT_CONN_CLOSED:
		/* retry pending requests if possible */
		error = _conn->input == NULL ? "Connection lost" :
			t_strdup_printf("Connection lost: %s",
					i_stream_get_error(_conn->input));
		http_client_connection_debug(conn, "%s", error);
		http_client_connection_retry_requests(conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST, error);
	default:
		break;
	}

	http_client_connection_unref(&conn);
}

static void http_client_payload_finished(struct http_client_connection *conn)
{
	timeout_remove(&conn->to_input);
	conn->conn.io = io_add_istream(conn->conn.input,
				       http_client_connection_input, &conn->conn);
}

static void
http_client_payload_destroyed_timeout(struct http_client_connection *conn)
{
	if (conn->close_indicated) {
		http_client_connection_server_close(&conn);
		return;
	}
	http_client_connection_input(&conn->conn);
}

static void http_client_payload_destroyed(struct http_client_request *req)
{
	struct http_client_connection *conn = req->conn;

	i_assert(conn != NULL);
	i_assert(conn->pending_request == req);
	i_assert(conn->incoming_payload != NULL);
	i_assert(conn->conn.io == NULL);

	http_client_connection_debug(conn,
		"Response payload stream destroyed (%u ms after initial response)",
		timeval_diff_msecs(&ioloop_timeval, &req->response_time));

	/* caller is allowed to change the socket fd to blocking while reading
	   the payload. make sure here that it's switched back. */
	net_set_nonblock(conn->conn.fd_in, TRUE);

	conn->incoming_payload = NULL;
	conn->pending_request = NULL;
	http_client_request_finish(&req);

	/* room for new requests */
	if (http_client_connection_is_ready(conn))
		http_client_peer_trigger_request_handler(conn->peer);

	/* input stream may have pending input. make sure input handler
	   gets called (but don't do it directly, since we get get here
	   somewhere from the API user's code, which we can't really know what
	   state it is in). this call also triggers sending a new request if
	   necessary. */
	conn->to_input =
		timeout_add_short(0, http_client_payload_destroyed_timeout, conn);

	i_assert(req != NULL);
	http_client_request_unref(&req);
}

static bool
http_client_connection_return_response(struct http_client_connection *conn,
	struct http_client_request *req, struct http_response *response)
{
	struct istream *payload;
	bool retrying;

	i_assert(!conn->in_req_callback);
	i_assert(conn->incoming_payload == NULL);
	i_assert(conn->pending_request == NULL);

	http_client_request_ref(req);
	req->state = HTTP_REQUEST_STATE_GOT_RESPONSE;

	if (response->payload != NULL) {
		/* wrap the stream to capture the destroy event without destroying the
		   actual payload stream. */
		conn->incoming_payload = response->payload =
			i_stream_create_timeout(response->payload,
				conn->client->set.request_timeout_msecs);
		i_stream_add_destroy_callback(response->payload,
					      http_client_payload_destroyed,
					      req);
		/* the callback may add its own I/O, so we need to remove
		   our one before calling it */
		io_remove(&conn->conn.io);
		/* we've received the request itself, and we can't reset the
		   timeout during the payload reading. */
		if (conn->to_requests != NULL)
			timeout_remove(&conn->to_requests);
	}
	
	conn->in_req_callback = TRUE;
	http_client_connection_ref(conn);
	retrying = !http_client_request_callback(req, response);
	http_client_connection_unref(&conn);
	conn->in_req_callback = FALSE;
	if (conn == NULL) {
		/* the callback managed to get this connection destroyed */
		if (!retrying)
			http_client_request_finish(&req);
		http_client_request_unref(&req);
		return FALSE;
	}

	if (retrying) {
		/* retrying, don't destroy the request */
		if (response->payload != NULL) {
			i_stream_remove_destroy_callback(conn->incoming_payload,
							 http_client_payload_destroyed);
			i_stream_unref(&conn->incoming_payload);
			conn->conn.io = io_add_istream(conn->conn.input,
					       http_client_connection_input,
					       &conn->conn);
		}
		http_client_request_unref(&req);
		return TRUE;
	}

	if (response->payload != NULL) {
		req->state = HTTP_REQUEST_STATE_PAYLOAD_IN;
		payload = response->payload;
		response->payload = NULL;
		conn->pending_request = req;

		/* request is dereferenced in payload destroy callback */
		i_stream_unref(&payload);

		if (conn->to_input != NULL) {
			/* already finished reading the payload */
			http_client_payload_finished(conn);
		}
	} else {
		http_client_request_finish(&req);
		http_client_request_unref(&req);
	}

	if (conn->incoming_payload == NULL) {
		i_assert(conn->conn.io != NULL ||
			conn->peer->addr.type == HTTP_CLIENT_PEER_ADDR_RAW);
		return TRUE;
	}

	return FALSE;
}

static void http_client_connection_input(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	struct http_response response;
	struct http_client_request *const *req_idx;
	struct http_client_request *req = NULL;
	int finished = 0, ret;
	const char *error;
	enum http_response_payload_type payload_type;

	i_assert(conn->incoming_payload == NULL);

	if (conn->to_input != NULL) {
		/* We came here from a timeout added by
		   http_client_payload_destroyed(). The IO couldn't be added
		   back immediately in there, because the HTTP API user may
		   still have had its own IO pointed to the same fd. It should
		   be removed by now, so we can add it back. */
		http_client_payload_finished(conn);
		finished++;
	}
	if (conn->to_requests != NULL)
		timeout_reset(conn->to_requests);

	/* get first waiting request */
	if (array_count(&conn->request_wait_list) > 0) {
		req_idx = array_idx(&conn->request_wait_list, 0);
		req = req_idx[0];

		/* determine whether to expect a response payload */
		payload_type = http_client_request_get_payload_type(req);
	} else {
		req = NULL;
		payload_type = HTTP_RESPONSE_PAYLOAD_TYPE_ALLOWED;
	}

	// FIXME: handle somehow if server replies before request->input is at EOF
	while ((ret=http_response_parse_next
		(conn->http_parser, payload_type, &response, &error)) > 0) {
		bool aborted;

		if (req == NULL) {
			/* server sent response without any requests in the wait list */
			http_client_connection_debug(conn, "Got unexpected input from server");
			http_client_connection_unref(&conn);
			return;
		}
		req->response_time = ioloop_timeval;

		/* Got some response; cancel response timeout */
		if (conn->to_response != NULL)
			timeout_remove(&conn->to_response);

		/* RFC 7231, Section 6.2:

		   A client MUST be able to parse one or more 1xx responses received
		   prior to a final response, even if the client does not expect one.  A
		   user agent MAY ignore unexpected 1xx responses.
		 */
		if (req->payload_sync && response.status == 100) {
			if (conn->payload_continue) {
				http_client_connection_debug(conn,
					"Got 100-continue response after timeout");
				continue;
			}
			conn->peer->no_payload_sync = FALSE;
			conn->peer->seen_100_response = TRUE;
			conn->payload_continue = TRUE;
			http_client_connection_debug(conn,
				"Got expected 100-continue response");
			if (http_client_request_send_more(req, &error) < 0) {
				http_client_connection_abort_temp_error(&conn,
					HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
					t_strdup_printf("Failed to send request: %s", error));
			}
			return;
		} else if (response.status / 100 == 1) {
			/* ignore them for now */
			http_client_connection_debug(conn,
				"Got unexpected %u response; ignoring", response.status);
			continue;
		} 

		http_client_connection_debug(conn,
			"Got %u response for request %s (took %u ms + %u ms in queue)",
			response.status, http_client_request_label(req),
			timeval_diff_msecs(&req->response_time, &req->sent_time),
			timeval_diff_msecs(&req->sent_time, &req->submit_time));

		/* make sure connection output is unlocked if 100-continue failed */
		if (req->payload_sync && !conn->payload_continue)
			conn->output_locked = FALSE;	

		/* remove request from queue */
		array_delete(&conn->request_wait_list, 0, 1);
		aborted = (req->state == HTTP_REQUEST_STATE_ABORTED);
		i_assert(req->refcount > 1 || aborted);
		http_client_request_unref(&req);
		
		conn->close_indicated = response.connection_close;

		if (!aborted) {
			bool handled = FALSE;

			/* don't redirect/retry if we're sending data in small
			   blocks via http_client_request_send_payload()
			   and we're not waiting for 100 continue */
			if (!req->payload_wait ||
				(req->payload_sync && !conn->payload_continue)) {
				/* failed Expect: */
				if (response.status == 417 && req->payload_sync) {
					/* drop Expect: continue */
					req->payload_sync = FALSE;
					conn->output_locked = FALSE;
					conn->peer->no_payload_sync = TRUE;
					if (http_client_request_try_retry(req))
						handled = TRUE;
				/* redirection */
				} else if (!req->client->set.no_auto_redirect &&
					response.status / 100 == 3 && response.status != 304 &&
					response.location != NULL) {
					/* redirect (possibly after delay) */
					if (http_client_request_delay_from_response(req, &response) >= 0) {
						http_client_request_redirect
							(req, response.status, response.location);
						handled = TRUE;
					}
				/* service unavailable */
				} else if (response.status == 503) {
					/* automatically retry after delay if indicated */
					if ( response.retry_after != (time_t)-1 &&
						http_client_request_delay_from_response(req, &response) > 0 &&
						http_client_request_try_retry(req))
						handled = TRUE;
				}
			}

			if (!handled) {
				/* response for application */
				if (!http_client_connection_return_response(conn, req, &response))
					return;
			}

			finished++;
		}

		/* server closing connection? */
		if (conn->close_indicated) {
			http_client_connection_server_close(&conn);
			return;
		}

		/* get next waiting request */
		if (array_count(&conn->request_wait_list) > 0) {
			req_idx = array_idx(&conn->request_wait_list, 0);
			req = req_idx[0];

			/* determine whether to expect a response payload */
			payload_type = http_client_request_get_payload_type(req);
		} else {
			/* no more requests waiting for the connection */
			if (conn->to_requests != NULL)
				timeout_remove(&conn->to_requests);
			req = NULL;
			payload_type = HTTP_RESPONSE_PAYLOAD_TYPE_ALLOWED;
		}
	}

	if (ret <= 0 &&
	    (conn->conn.input->eof || conn->conn.input->stream_errno != 0)) {
		int stream_errno = conn->conn.input->stream_errno;
		http_client_connection_abort_temp_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
			t_strdup_printf("Connection lost: read(%s) failed: %s",
					i_stream_get_name(conn->conn.input),
					stream_errno != 0 ?
					i_stream_get_error(conn->conn.input) :
					"EOF"));
		return;
	}

	if (ret < 0) {
		http_client_connection_abort_error(&conn,
			HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE, error);
		return;
	}

	if (finished > 0) {
		/* connection still alive after (at least one) request;
		   we can pipeline -> mark for subsequent connections */
		conn->peer->allows_pipelining = TRUE;

		/* room for new requests */
		if (http_client_connection_is_ready(conn))
			http_client_peer_trigger_request_handler(conn->peer);
	}
}

int http_client_connection_output(struct http_client_connection *conn)
{
	struct http_client_request *const *req_idx, *req;
	struct ostream *output = conn->conn.output;
	const char *error;
	int ret;

	if (conn->to_requests != NULL)
		timeout_reset(conn->to_requests);

	if ((ret = o_stream_flush(output)) <= 0) {
		if (ret < 0) {
			http_client_connection_abort_temp_error(&conn,
				HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
				t_strdup_printf("Connection lost: write(%s) failed: %s",
						o_stream_get_name(output),
						o_stream_get_error(output)));
		}
		return ret;
	}

	if (array_count(&conn->request_wait_list) > 0 && conn->output_locked) {
		req_idx = array_idx(&conn->request_wait_list,
			array_count(&conn->request_wait_list)-1);
		req = req_idx[0];

		if (!req->payload_sync || conn->payload_continue) {
			if (http_client_request_send_more(req, &error) < 0) {
				http_client_connection_abort_temp_error(&conn,
					HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST,
					t_strdup_printf("Connection lost: %s", error));
				return -1;
			}
			if (!conn->output_locked) {
				/* room for new requests */
				if (http_client_connection_is_ready(conn))
					http_client_peer_trigger_request_handler(conn->peer);
			}
		}
	}
	return 1;
}

void
http_client_connection_start_tunnel(struct http_client_connection **_conn,
	struct http_client_tunnel *tunnel)
{
	struct http_client_connection *conn = *_conn;

	i_assert(conn->tunneling);

	/* claim connection streams */
	memset(tunnel, 0, sizeof(*tunnel));
	tunnel->input = conn->conn.input;
	tunnel->output = conn->conn.output;
	tunnel->fd_in = conn->conn.fd_in;
	tunnel->fd_out = conn->conn.fd_out;

	/* detach from connection */
	conn->conn.input = NULL;
	conn->conn.output = NULL;
	conn->conn.fd_in = -1;
	conn->conn.fd_out = -1;
	conn->closing = TRUE;
	conn->connected = FALSE;
	connection_disconnect(&conn->conn);

	http_client_connection_unref(_conn);
}

static void 
http_client_connection_ready(struct http_client_connection *conn)
{
	/* connected */
	conn->connected = TRUE;
	conn->last_ioloop = current_ioloop;
	if (conn->to_connect != NULL &&
	    (conn->ssl_iostream == NULL ||
	     ssl_iostream_is_handshaked(conn->ssl_iostream)))
		timeout_remove(&conn->to_connect);

	/* indicate connection success */
	conn->connect_succeeded = TRUE;
	http_client_peer_connection_success(conn->peer);

	/* start raw log */
	if (conn->client->set.rawlog_dir != NULL) {
		iostream_rawlog_create(conn->client->set.rawlog_dir,
				       &conn->conn.input, &conn->conn.output);
	}

	/* direct tunneling connections handle connect requests just by providing a
	   raw connection */
	if (conn->peer->addr.type == HTTP_CLIENT_PEER_ADDR_RAW) {
		struct http_client_request *req;
		
		req = http_client_peer_claim_request(conn->peer, FALSE);
		if (req != NULL) {
			struct http_response response;

			http_client_request_ref(req);
			req->conn = conn;
			conn->tunneling = TRUE;

			memset(&response, 0, sizeof(response));
			response.status = 200;
			response.reason = "OK";

			(void)http_client_connection_return_response(conn, req, &response);
			http_client_request_unref(&req);
			return;
		} 
		
		http_client_connection_debug(conn,
			"No raw connect requests pending; closing useless connection");
		http_client_connection_unref(&conn);
		return;
	}

	/* start protocol I/O */
	conn->http_parser = http_response_parser_init
		(conn->conn.input, &conn->client->set.response_hdr_limits);
	o_stream_set_flush_callback(conn->conn.output,
    http_client_connection_output, conn);
}

static int
http_client_connection_ssl_handshaked(const char **error_r, void *context)
{
	struct http_client_connection *conn = context;
	const char *error, *host = conn->peer->addr.https_name;

	if (ssl_iostream_check_cert_validity(conn->ssl_iostream, host, &error) == 0)
		http_client_connection_debug(conn, "SSL handshake successful");
	else if (conn->client->set.ssl_allow_invalid_cert) {
		http_client_connection_debug(conn, "SSL handshake successful, "
			"ignoring invalid certificate: %s", error);
	} else {
		*error_r = error;
		return -1;
	}
	if (conn->to_connect != NULL)
		timeout_remove(&conn->to_connect);
	return 0;
}

static int 
http_client_connection_ssl_init(struct http_client_connection *conn,
				const char **error_r)
{
	struct ssl_iostream_settings ssl_set;
	const char *error;

	i_assert(conn->client->ssl_ctx != NULL);

	memset(&ssl_set, 0, sizeof(ssl_set));
	if (!conn->client->set.ssl_allow_invalid_cert) {
		ssl_set.verbose_invalid_cert = TRUE;
		ssl_set.verify_remote_cert = TRUE;
		ssl_set.require_valid_cert = TRUE;
	}

	if (conn->client->set.debug)
		http_client_connection_debug(conn, "Starting SSL handshake");

	if (io_stream_create_ssl_client(conn->client->ssl_ctx,
					conn->peer->addr.https_name, &ssl_set,
					&conn->conn.input, &conn->conn.output,
					&conn->ssl_iostream, &error) < 0) {
		*error_r = t_strdup_printf(
			"Couldn't initialize SSL client for %s: %s",
			conn->conn.name, error);
		return -1;
	}
	ssl_iostream_set_handshake_callback(conn->ssl_iostream,
					    http_client_connection_ssl_handshaked, conn);
	if (ssl_iostream_handshake(conn->ssl_iostream) < 0) {
		*error_r = t_strdup_printf("SSL handshake to %s failed: %s",
			conn->conn.name, ssl_iostream_get_last_error(conn->ssl_iostream));
		return -1;
	}

	http_client_connection_ready(conn);
	return 0;
}

static void 
http_client_connection_connected(struct connection *_conn, bool success)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	const char *error;

	if (!success) {
		http_client_peer_connection_failure(conn->peer, t_strdup_printf(
			"connect(%s) failed: %m", _conn->name));
	} else {
		conn->connected_timestamp = ioloop_timeval;
		http_client_connection_debug(conn, "Connected");
		if (conn->peer->addr.https_name != NULL) {
			if (http_client_connection_ssl_init(conn, &error) < 0) {
				http_client_peer_connection_failure(conn->peer, error);
				http_client_connection_debug(conn, "%s", error);
				http_client_connection_unref(&conn);
			}
			return;
		}
		http_client_connection_ready(conn);
	}
}

static const struct connection_settings http_client_connection_set = {
	.input_max_size = (size_t)-1,
	.output_max_size = (size_t)-1,
	.client = TRUE
};

static const struct connection_vfuncs http_client_connection_vfuncs = {
	.destroy = http_client_connection_destroy,
	.input = http_client_connection_input,
	.client_connected = http_client_connection_connected
};

struct connection_list *
http_client_connection_list_init(void)
{
	return connection_list_init
		(&http_client_connection_set, &http_client_connection_vfuncs);
}

static void
http_client_connection_delayed_connect_error(struct http_client_connection *conn)
{
	timeout_remove(&conn->to_input);
	errno = conn->connect_errno;
	http_client_connection_connected(&conn->conn, FALSE);
	http_client_connection_unref(&conn);
}

static void http_client_connect_timeout(struct http_client_connection *conn)
{
	conn->conn.disconnect_reason = CONNECTION_DISCONNECT_CONNECT_TIMEOUT;
	http_client_connection_destroy(&conn->conn);
}

static void
http_client_connection_connect(struct http_client_connection *conn)
{
	unsigned int msecs;

	conn->connect_start_timestamp = ioloop_timeval;
	if (connection_client_connect(&conn->conn) < 0) {
		conn->connect_errno = errno;
		http_client_connection_debug(conn, "Connect failed: %m");
		conn->to_input = timeout_add_short(0,
			http_client_connection_delayed_connect_error, conn);
		return;
	}

	/* don't use connection.h timeout because we want this timeout
	   to include also the SSL handshake */
	msecs = conn->client->set.connect_timeout_msecs;
	if (msecs == 0)
		msecs = conn->client->set.request_timeout_msecs;
	if (msecs > 0) {
		conn->to_connect =
			timeout_add(msecs, http_client_connect_timeout, conn);
	}
}

static void
http_client_connect_tunnel_timeout(struct http_client_connection *conn)
{
	http_client_connection_unref(&conn);
}

// FIXME: put something like this in lib/connection.c
static void
_connection_init_from_streams(struct connection_list *list,
			    struct connection *conn, const char *name,
			    struct istream *input, struct ostream *output)
{
	i_assert(name != NULL);

	conn->list = list;
	conn->name = i_strdup(name);
	conn->fd_in = i_stream_get_fd(input);
	conn->fd_out = o_stream_get_fd(output);

	i_assert(conn->fd_in >= 0);
	i_assert(conn->fd_out >= 0);
	i_assert(conn->io == NULL);
	i_assert(conn->input == NULL);
	i_assert(conn->output == NULL);
	i_assert(conn->to == NULL);

	conn->input = input;
	i_stream_set_name(conn->input, conn->name);

	conn->output = output;
	o_stream_set_no_error_handling(conn->output, TRUE);
	o_stream_set_name(conn->output, conn->name);

	conn->io = io_add_istream(conn->input, *list->v.input, conn);
	
	DLLIST_PREPEND(&list->connections, conn);
	list->connections_count++;

	if (list->v.client_connected != NULL)
		list->v.client_connected(conn, TRUE);
}

static void
http_client_connection_tunnel_response(const struct http_response *response,
			       struct http_client_connection *conn)
{
	struct http_client_tunnel tunnel;
	const char *name = http_client_peer_addr2str(&conn->peer->addr);

	if (response->status != 200) {
		http_client_peer_connection_failure(conn->peer, t_strdup_printf(
			"tunnel connect(%s) failed: %d %s", name,
				response->status, response->reason));
		conn->connect_request = NULL;
		return;
	}

	http_client_request_start_tunnel(conn->connect_request, &tunnel);
	conn->connect_request = NULL;

	_connection_init_from_streams
		(conn->client->conn_list, &conn->conn, name, tunnel.input, tunnel.output);
}

static void
http_client_connection_connect_tunnel(struct http_client_connection *conn,
	const struct ip_addr *ip, unsigned int port)
{
	unsigned int msecs;

	conn->connect_start_timestamp = ioloop_timeval;

	conn->connect_request = http_client_request_connect_ip
		(conn->client, ip, port, http_client_connection_tunnel_response, conn);
	http_client_request_set_urgent(conn->connect_request);
	http_client_request_submit(conn->connect_request);

	/* don't use connection.h timeout because we want this timeout
	   to include also the SSL handshake */
	msecs = conn->client->set.connect_timeout_msecs;
	if (msecs == 0)
		msecs = conn->client->set.request_timeout_msecs;
	if (msecs > 0) {
		conn->to_connect =
			timeout_add(msecs, http_client_connect_tunnel_timeout, conn);
	}
}

struct http_client_connection *
http_client_connection_create(struct http_client_peer *peer)
{
	struct http_client_connection *conn;
	static unsigned int id = 0;
	const struct http_client_peer_addr *addr = &peer->addr;
	const char *conn_type = "UNKNOWN";

	switch (peer->addr.type) {
	case HTTP_CLIENT_PEER_ADDR_HTTP:
		conn_type = "HTTP";
		break;
	case HTTP_CLIENT_PEER_ADDR_HTTPS:
		conn_type = "HTTPS";
		break;
	case HTTP_CLIENT_PEER_ADDR_HTTPS_TUNNEL:
		conn_type = "Tunneled HTTPS";
		break;
	case HTTP_CLIENT_PEER_ADDR_RAW:
		conn_type = "Raw";
		break;
	}

	conn = i_new(struct http_client_connection, 1);
	conn->refcount = 1;
	conn->client = peer->client;
	conn->id = id++;
	conn->peer = peer;
	if (peer->addr.type != HTTP_CLIENT_PEER_ADDR_RAW)
		i_array_init(&conn->request_wait_list, 16);

	if (peer->addr.type == HTTP_CLIENT_PEER_ADDR_HTTPS_TUNNEL) {
		http_client_connection_connect_tunnel(conn, &addr->ip, addr->port);
	} else {
		connection_init_client_ip
			(peer->client->conn_list, &conn->conn, &addr->ip, addr->port);
		http_client_connection_connect(conn);
	}

	array_append(&peer->conns, &conn, 1);

	http_client_connection_debug(conn,
		"%s connection created (%d parallel connections exist)%s",
		conn_type, array_count(&peer->conns),
		(conn->to_input == NULL ? "" : " [broken]"));
	return conn;
}

void http_client_connection_ref(struct http_client_connection *conn)
{
	conn->refcount++;
}

static void
http_client_connection_disconnect(struct http_client_connection *conn)
{
	conn->closing = TRUE;
	conn->connected = FALSE;

	if (conn->connect_request != NULL)
		http_client_request_abort(&conn->connect_request);

	if (conn->incoming_payload != NULL) {
		/* the stream is still accessed by lib-http caller. */
		i_stream_remove_destroy_callback(conn->incoming_payload,
						 http_client_payload_destroyed);
		conn->incoming_payload = NULL;
	}

	connection_disconnect(&conn->conn);

	if (conn->io_req_payload != NULL)
		io_remove(&conn->io_req_payload);
	if (conn->to_requests != NULL)
		timeout_remove(&conn->to_requests);
	if (conn->to_connect != NULL)
		timeout_remove(&conn->to_connect);
	if (conn->to_input != NULL)
		timeout_remove(&conn->to_input);
	if (conn->to_idle != NULL)
		timeout_remove(&conn->to_idle);
	if (conn->to_response != NULL)
		timeout_remove(&conn->to_response);
}

void http_client_connection_unref(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_connection *const *conn_idx;
	ARRAY_TYPE(http_client_connection) *conn_arr;
	struct http_client_peer *peer = conn->peer;
	struct http_client_request **req;

	i_assert(conn->refcount > 0);

	if (--conn->refcount > 0)
		return;

	http_client_connection_debug(conn, "Connection destroy");

	http_client_connection_disconnect(conn);

	if (array_is_created(&conn->request_wait_list)) {
		/* abort all pending requests */
		array_foreach_modifiable(&conn->request_wait_list, req) {
			i_assert((*req)->submitted);
			http_client_request_error(*req, HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Aborting");
		}
		array_free(&conn->request_wait_list);
	}
	if (conn->pending_request != NULL) {
		http_client_request_error(conn->pending_request,
			HTTP_CLIENT_REQUEST_ERROR_ABORTED, "Aborting");
	}

	if (conn->http_parser != NULL)
		http_response_parser_deinit(&conn->http_parser);

	if (conn->ssl_iostream != NULL)
		ssl_iostream_unref(&conn->ssl_iostream);
	connection_deinit(&conn->conn);
	
	/* remove this connection from the list */
	conn_arr = &conn->peer->conns;
	array_foreach(conn_arr, conn_idx) {
		if (*conn_idx == conn) {
			array_delete(conn_arr, array_foreach_idx(conn_arr, conn_idx), 1);
			break;
		}
	}

	if (conn->connect_succeeded)
		http_client_peer_connection_lost(peer);
	i_free(conn);
	*_conn = NULL;
}

void http_client_connection_switch_ioloop(struct http_client_connection *conn)
{
	if (conn->io_req_payload != NULL)
		conn->io_req_payload = io_loop_move_io(&conn->io_req_payload);
	if (conn->to_requests != NULL)
		conn->to_requests = io_loop_move_timeout(&conn->to_requests);
	if (conn->to_connect != NULL)
		conn->to_connect = io_loop_move_timeout(&conn->to_connect);
	if (conn->to_input != NULL)
		conn->to_input = io_loop_move_timeout(&conn->to_input);
	if (conn->to_idle != NULL)
		conn->to_idle = io_loop_move_timeout(&conn->to_idle);
	if (conn->to_response != NULL)
		conn->to_response = io_loop_move_timeout(&conn->to_response);
	if (conn->incoming_payload != NULL)
		i_stream_switch_ioloop(conn->incoming_payload);
	connection_switch_ioloop(&conn->conn);
}
