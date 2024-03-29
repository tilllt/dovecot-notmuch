/* Copyright (c) 2005-2014 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "base64.h"
#include "ioloop.h"
#include "net.h"
#include "istream.h"
#include "ostream.h"
#include "hex-binary.h"
#include "str.h"
#include "strescape.h"
#include "process-title.h"
#include "master-service.h"
#include "auth-request.h"
#include "auth-worker-client.h"

#include <stdlib.h>

#define AUTH_WORKER_WARN_DISCONNECTED_LONG_CMD_SECS 30
#define OUTBUF_THROTTLE_SIZE (1024*10)

#define CLIENT_STATE_HANDSHAKE "handshaking"
#define CLIENT_STATE_ITER "iterating users"
#define CLIENT_STATE_IDLE "idling"
#define CLIENT_STATE_STOP "waiting for shutdown"

struct auth_worker_client {
	int refcount;

        struct auth *auth;
	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct timeout *to_idle;
	time_t cmd_start;

	unsigned int version_received:1;
	unsigned int dbhash_received:1;
	unsigned int error_sent:1;
};

struct auth_worker_list_context {
	struct auth_worker_client *client;
	struct auth_request *auth_request;
	struct userdb_iterate_context *iter;
	bool sending, sent, done;
};

struct auth_worker_client *auth_worker_client;
static bool auth_worker_client_error = FALSE;

static void auth_worker_input(struct auth_worker_client *client);
static int auth_worker_output(struct auth_worker_client *client);

void auth_worker_refresh_proctitle(const char *state)
{
	if (!global_auth_settings->verbose_proctitle || !worker)
		return;

	if (auth_worker_client_error)
		state = "error";
	else if (auth_worker_client == NULL)
		state = "waiting for connection";
	process_title_set(t_strdup_printf("worker: %s", state));
}

static void
auth_worker_client_check_throttle(struct auth_worker_client *client)
{
	if (o_stream_get_buffer_used_size(client->output) >=
	    OUTBUF_THROTTLE_SIZE) {
		/* stop reading new requests until client has read the pending
		   replies. */
		if (client->io != NULL)
			io_remove(&client->io);
	}
}

static struct auth_request *
worker_auth_request_new(struct auth_worker_client *client, unsigned int id,
			const char *const *args)
{
	struct auth_request *auth_request;
	const char *key, *value;

	auth_request = auth_request_new_dummy();

	client->refcount++;
	auth_request->context = client;
	auth_request->id = id;

	for (; *args != NULL; args++) {
		value = strchr(*args, '=');
		if (value == NULL)
			(void)auth_request_import(auth_request, *args, NULL);
		else {
			key = t_strdup_until(*args, value++);
			(void)auth_request_import(auth_request, key, value);
		}
	}

	auth_request_init(auth_request);
	return auth_request;
}

static void auth_worker_send_reply(struct auth_worker_client *client,
				   struct auth_request *request,
				   string_t *str)
{
	time_t cmd_duration = time(NULL) - client->cmd_start;
	const char *p;

	if (worker_restart_request)
		o_stream_nsend_str(client->output, "RESTART\n");
	o_stream_nsend(client->output, str_data(str), str_len(str));
	if (o_stream_nfinish(client->output) < 0 && request != NULL &&
	    cmd_duration > AUTH_WORKER_WARN_DISCONNECTED_LONG_CMD_SECS) {
		p = strchr(str_c(str), '\t');
		p = p == NULL ? "BUG" : t_strcut(p+1, '\t');

		i_warning("Auth master disconnected us while handling "
			  "request for %s for %ld secs (result=%s)",
			  request->user, (long)cmd_duration, p);
	}
}

static void
reply_append_extra_fields(string_t *str, struct auth_request *request)
{
	if (!auth_fields_is_empty(request->extra_fields)) {
		str_append_c(str, '\t');
		auth_fields_append(request->extra_fields, str, 0, 0);
	}
	if (request->userdb_reply != NULL &&
	    auth_fields_is_empty(request->userdb_reply)) {
		/* all userdb_* fields had NULL values. we'll still
		   need to tell this to the master */
		str_append(str, "\tuserdb_"AUTH_REQUEST_USER_KEY_IGNORE);
	}
}

static void verify_plain_callback(enum passdb_result result,
				  struct auth_request *request)
{
	struct auth_worker_client *client = request->context;
	string_t *str;

	if (request->failed && result == PASSDB_RESULT_OK)
		result = PASSDB_RESULT_PASSWORD_MISMATCH;

	str = t_str_new(128);
	str_printfa(str, "%u\t", request->id);

	if (result == PASSDB_RESULT_OK)
		str_append(str, "OK");
	else
		str_printfa(str, "FAIL\t%d", result);
	if (result != PASSDB_RESULT_INTERNAL_FAILURE) {
		str_append_c(str, '\t');
		str_append_tabescaped(str, request->user);
		str_append_c(str, '\t');
		if (request->passdb_password != NULL)
			str_append_tabescaped(str, request->passdb_password);
		reply_append_extra_fields(str, request);
	}
	str_append_c(str, '\n');
	auth_worker_send_reply(client, request, str);

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static bool
auth_worker_handle_passv(struct auth_worker_client *client,
			 unsigned int id, const char *const *args)
{
	/* verify plaintext password */
	struct auth_request *auth_request;
        struct auth_passdb *passdb;
	const char *password;
	unsigned int passdb_id;

	/* <passdb id> <password> [<args>] */
	if (str_to_uint(args[0], &passdb_id) < 0 || args[1] == NULL) {
		i_error("BUG: Auth worker server sent us invalid PASSV");
		return FALSE;
	}
	password = args[1];

	auth_request = worker_auth_request_new(client, id, args + 2);
	auth_request->mech_password =
		p_strdup(auth_request->pool, password);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: PASSV had missing parameters");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	passdb = auth_request->passdb;
	while (passdb != NULL && passdb->passdb->id != passdb_id)
		passdb = passdb->next;

	if (passdb == NULL) {
		/* could be a masterdb */
		passdb = auth_request_get_auth(auth_request)->masterdbs;
		while (passdb != NULL && passdb->passdb->id != passdb_id)
			passdb = passdb->next;

		if (passdb == NULL) {
			i_error("BUG: PASSV had invalid passdb ID");
			auth_request_unref(&auth_request);
			return FALSE;
		}
	}

	auth_request->passdb = passdb;
	passdb->passdb->iface.
		verify_plain(auth_request, password, verify_plain_callback);
	return TRUE;
}

static void
lookup_credentials_callback(enum passdb_result result,
			    const unsigned char *credentials, size_t size,
			    struct auth_request *request)
{
	struct auth_worker_client *client = request->context;
	string_t *str;

	if (request->failed && result == PASSDB_RESULT_OK)
		result = PASSDB_RESULT_PASSWORD_MISMATCH;

	str = t_str_new(128);
	str_printfa(str, "%u\t", request->id);

	if (result != PASSDB_RESULT_OK)
		str_printfa(str, "FAIL\t%d", result);
	else {
		str_append(str, "OK\t");
		str_append_tabescaped(str, request->user);
		str_printfa(str, "\t{%s.b64}", request->credentials_scheme);
		base64_encode(credentials, size, str);
		reply_append_extra_fields(str, request);
	}
	str_append_c(str, '\n');
	auth_worker_send_reply(client, request, str);

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static bool
auth_worker_handle_passl(struct auth_worker_client *client,
			 unsigned int id, const char *const *args)
{
	/* lookup credentials */
	struct auth_request *auth_request;
	const char *scheme;
	unsigned int passdb_id;

	/* <passdb id> <scheme> [<args>] */
	if (str_to_uint(args[0], &passdb_id) < 0 || args[1] == NULL) {
		i_error("BUG: Auth worker server sent us invalid PASSL");
		return FALSE;
	}
	scheme = args[1];

	auth_request = worker_auth_request_new(client, id, args + 2);
	auth_request->credentials_scheme = p_strdup(auth_request->pool, scheme);

	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: PASSL had missing parameters");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	while (auth_request->passdb->passdb->id != passdb_id) {
		auth_request->passdb = auth_request->passdb->next;
		if (auth_request->passdb == NULL) {
			i_error("BUG: PASSL had invalid passdb ID");
			auth_request_unref(&auth_request);
			return FALSE;
		}
	}

	if (auth_request->passdb->passdb->iface.lookup_credentials == NULL) {
		i_error("BUG: PASSL lookup not supported by given passdb");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	auth_request->prefer_plain_credentials = TRUE;
	auth_request->passdb->passdb->iface.
		lookup_credentials(auth_request, lookup_credentials_callback);
	return TRUE;
}

static void
set_credentials_callback(bool success, struct auth_request *request)
{
	struct auth_worker_client *client = request->context;

	string_t *str;

	str = t_str_new(64);
	str_printfa(str, "%u\t%s\n", request->id, success ? "OK" : "FAIL");
	auth_worker_send_reply(client, request, str);

	auth_request_unref(&request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static bool
auth_worker_handle_setcred(struct auth_worker_client *client,
			   unsigned int id, const char *const *args)
{
	struct auth_request *auth_request;
	unsigned int passdb_id;
	const char *creds;

	/* <passdb id> <credentials> [<args>] */
	if (str_to_uint(args[0], &passdb_id) < 0 || args[1] == NULL) {
		i_error("BUG: Auth worker server sent us invalid SETCRED");
		return FALSE;
	}
	creds = args[1];

	auth_request = worker_auth_request_new(client, id, args + 2);
	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: SETCRED had missing parameters");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	while (auth_request->passdb->passdb->id != passdb_id) {
		auth_request->passdb = auth_request->passdb->next;
		if (auth_request->passdb == NULL) {
			i_error("BUG: SETCRED had invalid passdb ID");
			auth_request_unref(&auth_request);
			return FALSE;
		}
	}

	auth_request->passdb->passdb->iface.
		set_credentials(auth_request, creds, set_credentials_callback);
	return TRUE;
}

static void
lookup_user_callback(enum userdb_result result,
		     struct auth_request *auth_request)
{
	struct auth_worker_client *client = auth_request->context;
	string_t *str;

	str = t_str_new(128);
	str_printfa(str, "%u\t", auth_request->id);
	switch (result) {
	case USERDB_RESULT_INTERNAL_FAILURE:
		str_append(str, "FAIL\t");
		break;
	case USERDB_RESULT_USER_UNKNOWN:
		str_append(str, "NOTFOUND\t");
		break;
	case USERDB_RESULT_OK:
		str_append(str, "OK\t");
		auth_fields_append(auth_request->userdb_reply, str, 0, 0);
		if (auth_request->userdb_lookup_tempfailed)
			str_append(str, "\ttempfail");
		break;
	}
	str_append_c(str, '\n');

	auth_worker_send_reply(client, auth_request, str);

	auth_request_unref(&auth_request);
	auth_worker_client_check_throttle(client);
	auth_worker_client_unref(&client);
}

static struct auth_userdb *
auth_userdb_find_by_id(struct auth_userdb *userdbs, unsigned int id)
{
	struct auth_userdb *db;

	for (db = userdbs; db != NULL; db = db->next) {
		if (db->userdb->id == id)
			return db;
	}
	return NULL;
}

static bool
auth_worker_handle_user(struct auth_worker_client *client,
			unsigned int id, const char *const *args)
{
	/* lookup user */
	struct auth_request *auth_request;
	unsigned int userdb_id;

	/* <userdb id> [<args>] */
	if (str_to_uint(args[0], &userdb_id) < 0) {
		i_error("BUG: Auth worker server sent us invalid USER");
		return FALSE;
	}

	auth_request = worker_auth_request_new(client, id, args + 1);
	if (auth_request->user == NULL || auth_request->service == NULL) {
		i_error("BUG: USER had missing parameters");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	auth_request->userdb =
		auth_userdb_find_by_id(auth_request->userdb, userdb_id);
	if (auth_request->userdb == NULL) {
		i_error("BUG: USER had invalid userdb ID");
		auth_request_unref(&auth_request);
		return FALSE;
	}

	auth_request_init_userdb_reply(auth_request);
	auth_request->userdb->userdb->iface->
		lookup(auth_request, lookup_user_callback);
	return TRUE;
}

static void
auth_worker_client_idle_kill(struct auth_worker_client *client ATTR_UNUSED)
{
	auth_worker_client_send_shutdown();
}

static void
auth_worker_client_set_idle_timeout(struct auth_worker_client *client)
{
	unsigned int idle_kill_secs;

	i_assert(client->to_idle == NULL);

	idle_kill_secs = master_service_get_idle_kill_secs(master_service);
	if (idle_kill_secs > 0) {
		client->to_idle = timeout_add(idle_kill_secs * 1000,
					      auth_worker_client_idle_kill,
					      client);
	}
}

static void list_iter_deinit(struct auth_worker_list_context *ctx)
{
	struct auth_worker_client *client = ctx->client;
	string_t *str;

	i_assert(client->io == NULL);

	str = t_str_new(32);
	if (ctx->auth_request->userdb->userdb->iface->
	    		iterate_deinit(ctx->iter) < 0)
		str_printfa(str, "%u\tFAIL\n", ctx->auth_request->id);
	else
		str_printfa(str, "%u\tOK\n", ctx->auth_request->id);
	auth_worker_send_reply(client, NULL, str);

	client->io = io_add(client->fd, IO_READ, auth_worker_input, client);
	auth_worker_client_set_idle_timeout(client);

	o_stream_set_flush_callback(client->output, auth_worker_output, client);
	auth_request_unref(&ctx->auth_request);
	auth_worker_client_unref(&client);
	i_free(ctx);

	auth_worker_refresh_proctitle(CLIENT_STATE_IDLE);
}

static void list_iter_callback(const char *user, void *context)
{
	struct auth_worker_list_context *ctx = context;
	string_t *str;

	if (user == NULL) {
		if (ctx->sending)
			ctx->done = TRUE;
		else
			list_iter_deinit(ctx);
		return;
	}

	if (!ctx->sending)
		o_stream_cork(ctx->client->output);
	T_BEGIN {
		str = t_str_new(128);
		str_printfa(str, "%u\t*\t%s\n", ctx->auth_request->id, user);
		o_stream_nsend(ctx->client->output, str_data(str), str_len(str));
	} T_END;

	if (ctx->sending) {
		/* avoid recursively looping to this same function */
		ctx->sent = TRUE;
		return;
	}

	do {
		ctx->sending = TRUE;
		ctx->sent = FALSE;
		T_BEGIN {
			ctx->auth_request->userdb->userdb->iface->
				iterate_next(ctx->iter);
		} T_END;
		if (o_stream_get_buffer_used_size(ctx->client->output) > OUTBUF_THROTTLE_SIZE) {
			if (o_stream_flush(ctx->client->output) < 0) {
				ctx->done = TRUE;
				break;
			}
		}
	} while (ctx->sent &&
		 o_stream_get_buffer_used_size(ctx->client->output) <= OUTBUF_THROTTLE_SIZE);
	o_stream_uncork(ctx->client->output);
	ctx->sending = FALSE;
	if (ctx->done)
		list_iter_deinit(ctx);
	else
		o_stream_set_flush_pending(ctx->client->output, TRUE);
}

static int auth_worker_list_output(struct auth_worker_list_context *ctx)
{
	int ret;

	if ((ret = o_stream_flush(ctx->client->output)) < 0) {
		list_iter_deinit(ctx);
		return 1;
	}
	if (ret > 0) T_BEGIN {
		ctx->auth_request->userdb->userdb->iface->
			iterate_next(ctx->iter);
	} T_END;
	return 1;
}

static bool
auth_worker_handle_list(struct auth_worker_client *client,
			unsigned int id, const char *const *args)
{
	struct auth_worker_list_context *ctx;
	struct auth_userdb *userdb;
	unsigned int userdb_id;

	if (str_to_uint(args[0], &userdb_id) < 0) {
		i_error("BUG: Auth worker server sent us invalid LIST");
		return FALSE;
	}

	userdb = auth_userdb_find_by_id(client->auth->userdbs, userdb_id);
	if (userdb == NULL) {
		i_error("BUG: LIST had invalid userdb ID");
		return FALSE;
	}

	ctx = i_new(struct auth_worker_list_context, 1);
	ctx->client = client;
	ctx->auth_request = worker_auth_request_new(client, id, args + 1);
	ctx->auth_request->userdb = userdb;
	if (ctx->auth_request->user == NULL ||
	    ctx->auth_request->service == NULL) {
		i_error("BUG: LIST had missing parameters");
		auth_request_unref(&ctx->auth_request);
		i_free(ctx);
		return FALSE;
	}

	io_remove(&ctx->client->io);
	if (ctx->client->to_idle != NULL)
		timeout_remove(&ctx->client->to_idle);

	o_stream_set_flush_callback(ctx->client->output,
				    auth_worker_list_output, ctx);
	ctx->iter = ctx->auth_request->userdb->userdb->iface->
		iterate_init(ctx->auth_request, list_iter_callback, ctx);
	ctx->auth_request->userdb->userdb->iface->iterate_next(ctx->iter);
	return TRUE;
}

static bool
auth_worker_handle_line(struct auth_worker_client *client, const char *line)
{
	const char *const *args;
	unsigned int id;
	bool ret = FALSE;

	args = t_strsplit_tab(line);
	if (args[0] == NULL || args[1] == NULL || args[2] == NULL ||
	    str_to_uint(args[0], &id) < 0) {
		i_error("BUG: Invalid input: %s", line);
		return FALSE;
	}

	io_loop_time_refresh();
	client->cmd_start = ioloop_time;

	auth_worker_refresh_proctitle(args[1]);
	if (strcmp(args[1], "PASSV") == 0)
		ret = auth_worker_handle_passv(client, id, args + 2);
	else if (strcmp(args[1], "PASSL") == 0)
		ret = auth_worker_handle_passl(client, id, args + 2);
	else if (strcmp(args[1], "SETCRED") == 0)
		ret = auth_worker_handle_setcred(client, id, args + 2);
	else if (strcmp(args[1], "USER") == 0)
		ret = auth_worker_handle_user(client, id, args + 2);
	else if (strcmp(args[1], "LIST") == 0)
		ret = auth_worker_handle_list(client, id, args + 2);
	else {
		i_error("BUG: Auth-worker received unknown command: %s",
			args[1]);
	}
	if (client->io != NULL)
		auth_worker_refresh_proctitle(CLIENT_STATE_IDLE);
        return ret;
}

static bool auth_worker_verify_db_hash(const char *line)
{
	string_t *str;
	unsigned char passdb_md5[MD5_RESULTLEN];
	unsigned char userdb_md5[MD5_RESULTLEN];

	passdbs_generate_md5(passdb_md5);
	userdbs_generate_md5(userdb_md5);

	str = t_str_new(128);
	str_append(str, "DBHASH\t");
	binary_to_hex_append(str, passdb_md5, sizeof(passdb_md5));
	str_append_c(str, '\t');
	binary_to_hex_append(str, userdb_md5, sizeof(userdb_md5));

	return strcmp(line, str_c(str)) == 0;
}

static void auth_worker_input(struct auth_worker_client *client)
{
	char *line;
	bool ret;

	switch (i_stream_read(client->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		auth_worker_client_destroy(&client);
		return;
	case -2:
		/* buffer full */
		i_error("BUG: Auth worker server sent us more than %d bytes",
			(int)AUTH_WORKER_MAX_LINE_LENGTH);
		auth_worker_client_destroy(&client);
		return;
	}

	if (!client->version_received) {
		line = i_stream_next_line(client->input);
		if (line == NULL)
			return;

		if (!version_string_verify(line, "auth-worker",
				     AUTH_WORKER_PROTOCOL_MAJOR_VERSION)) {
			i_error("Auth worker not compatible with this server "
				"(mixed old and new binaries?)");
			auth_worker_client_destroy(&client);
			return;
		}
		client->version_received = TRUE;
	}
	if (!client->dbhash_received) {
		line = i_stream_next_line(client->input);
		if (line == NULL)
			return;

		if (!auth_worker_verify_db_hash(line)) {
			i_error("Auth worker sees different passdbs/userdbs "
				"than auth server. Maybe config just changed "
				"and this goes away automatically?");
			auth_worker_client_destroy(&client);
			return;
		}
		client->dbhash_received = TRUE;
		auth_worker_refresh_proctitle(CLIENT_STATE_IDLE);
	}

        client->refcount++;
	while ((line = i_stream_next_line(client->input)) != NULL) {
		T_BEGIN {
			ret = auth_worker_handle_line(client, line);
		} T_END;

		if (!ret) {
			struct auth_worker_client *client2 = client;
			auth_worker_client_destroy(&client2);
			break;
		}
	}
	if (client->to_idle != NULL)
		timeout_reset(client->to_idle);
	auth_worker_client_unref(&client);
}

static int auth_worker_output(struct auth_worker_client *client)
{
	if (o_stream_flush(client->output) < 0) {
		auth_worker_client_destroy(&client);
		return 1;
	}

	if (o_stream_get_buffer_used_size(client->output) <=
	    OUTBUF_THROTTLE_SIZE/3 && client->io == NULL) {
		/* allow input again */
		client->io = io_add(client->fd, IO_READ,
				    auth_worker_input, client);
	}
	return 1;
}

struct auth_worker_client *
auth_worker_client_create(struct auth *auth, int fd)
{
        struct auth_worker_client *client;

	client = i_new(struct auth_worker_client, 1);
	client->refcount = 1;

	client->auth = auth;
	client->fd = fd;
	client->input = i_stream_create_fd(fd, AUTH_WORKER_MAX_LINE_LENGTH,
					   FALSE);
	client->output = o_stream_create_fd(fd, (size_t)-1, FALSE);
	o_stream_set_no_error_handling(client->output, TRUE);
	o_stream_set_flush_callback(client->output, auth_worker_output, client);
	client->io = io_add(fd, IO_READ, auth_worker_input, client);
	auth_worker_client_set_idle_timeout(client);
	auth_worker_refresh_proctitle(CLIENT_STATE_HANDSHAKE);

	auth_worker_client = client;
	if (auth_worker_client_error)
		auth_worker_client_send_error();
	return client;
}

void auth_worker_client_destroy(struct auth_worker_client **_client)
{
	struct auth_worker_client *client = *_client;

	*_client = NULL;
	if (client->fd == -1)
		return;

	i_stream_close(client->input);
	o_stream_close(client->output);

	if (client->to_idle != NULL)
		timeout_remove(&client->to_idle);
	if (client->io != NULL)
		io_remove(&client->io);

	net_disconnect(client->fd);
	client->fd = -1;
	auth_worker_client_unref(&client);

	auth_worker_client = NULL;
	auth_worker_refresh_proctitle("");
	master_service_client_connection_destroyed(master_service);
}

void auth_worker_client_unref(struct auth_worker_client **_client)
{
	struct auth_worker_client *client = *_client;

	*_client = NULL;

	if (--client->refcount > 0)
		return;

	i_stream_unref(&client->input);
	o_stream_unref(&client->output);
	i_free(client);
}

void auth_worker_client_send_error(void)
{
	auth_worker_client_error = TRUE;
	if (auth_worker_client != NULL &&
	    !auth_worker_client->error_sent) {
		o_stream_nsend_str(auth_worker_client->output, "ERROR\n");
		auth_worker_client->error_sent = TRUE;
	}
	auth_worker_refresh_proctitle("");
}

void auth_worker_client_send_success(void)
{
	auth_worker_client_error = FALSE;
	if (auth_worker_client == NULL)
		return;
	if (auth_worker_client->error_sent) {
		o_stream_nsend_str(auth_worker_client->output, "SUCCESS\n");
		auth_worker_client->error_sent = FALSE;
	}
	if (auth_worker_client->io != NULL)
		auth_worker_refresh_proctitle(CLIENT_STATE_IDLE);
}

void auth_worker_client_send_shutdown(void)
{
	if (auth_worker_client != NULL)
		o_stream_nsend_str(auth_worker_client->output, "SHUTDOWN\n");
	auth_worker_refresh_proctitle(CLIENT_STATE_STOP);
}
