/* Copyright (c) 2004-2014 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "llist.h"
#include "str.h"
#include "str-sanitize.h"
#include "time-util.h"
#include "master-service.h"
#include "ipc-server.h"
#include "mail-user-hash.h"
#include "client-common.h"
#include "ssl-proxy.h"
#include "login-proxy-state.h"
#include "login-proxy.h"

#define MAX_PROXY_INPUT_SIZE 4096
#define OUTBUF_THRESHOLD 1024
#define LOGIN_PROXY_DIE_IDLE_SECS 2
#define LOGIN_PROXY_IPC_PATH "ipc-proxy"
#define LOGIN_PROXY_IPC_NAME "proxy"
#define KILLED_BY_ADMIN_REASON "Killed by admin"
#define PROXY_IMMEDIATE_FAILURE_SECS 30

struct login_proxy {
	struct login_proxy *prev, *next;

	struct client *client;
	int client_fd, server_fd;
	struct io *client_io, *server_io;
	struct istream *server_input;
	struct ostream *client_output, *server_output;
	struct ssl_proxy *ssl_server_proxy;
	time_t last_io;

	struct timeval created;
	struct timeout *to, *to_notify;
	struct login_proxy_record *state_rec;

	struct ip_addr ip, source_ip;
	char *host;
	unsigned int port;
	unsigned int connect_timeout_msecs;
	unsigned int notify_refresh_secs;
	enum login_proxy_ssl_flags ssl_flags;

	proxy_callback_t *callback;

	unsigned int connected:1;
	unsigned int destroying:1;
	unsigned int disconnecting:1;
};

static struct login_proxy_state *proxy_state;
static struct login_proxy *login_proxies = NULL;
static struct login_proxy *login_proxies_pending = NULL;
static struct ipc_server *login_proxy_ipc_server;

static void login_proxy_ipc_cmd(struct ipc_cmd *cmd, const char *line);

static void
login_proxy_free_reason(struct login_proxy **_proxy, const char *reason)
	ATTR_NULL(2);

static void login_proxy_free_errno(struct login_proxy **proxy,
				   int err, const char *who)
{
	const char *reason;

	reason = err == 0 || err == EPIPE ?
		t_strdup_printf("Disconnected by %s", who) :
		t_strdup_printf("Disconnected by %s: %s", who, strerror(errno));
	login_proxy_free_reason(proxy, reason);
}

static void server_input(struct login_proxy *proxy)
{
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret, ret2;

	proxy->last_io = ioloop_time;
	if (o_stream_get_buffer_used_size(proxy->client_output) >
	    OUTBUF_THRESHOLD) {
		/* client's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(&proxy->server_io);
		return;
	}

	ret = net_receive(proxy->server_fd, buf, sizeof(buf));
	if (ret < 0) {
		login_proxy_free_errno(&proxy, errno, "server");
		return;
	}
	o_stream_cork(proxy->client_output);
	ret2 = o_stream_send(proxy->client_output, buf, ret);
	o_stream_uncork(proxy->client_output);
	if (ret2 != ret) {
		login_proxy_free_errno(&proxy,
				       proxy->client_output->stream_errno,
				       "client");
	}
}

static void proxy_client_input(struct login_proxy *proxy)
{
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret, ret2;

	proxy->last_io = ioloop_time;
	if (o_stream_get_buffer_used_size(proxy->server_output) >
	    OUTBUF_THRESHOLD) {
		/* proxy's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(&proxy->client_io);
		return;
	}

	ret = net_receive(proxy->client_fd, buf, sizeof(buf));
	if (ret < 0) {
		login_proxy_free_errno(&proxy, errno, "client");
		return;
	}
	o_stream_cork(proxy->server_output);
	ret2 = o_stream_send(proxy->server_output, buf, ret);
	o_stream_uncork(proxy->server_output);
	if (ret2 != ret) {
		login_proxy_free_errno(&proxy,
				       proxy->server_output->stream_errno,
				       "server");
	}
}

static int server_output(struct login_proxy *proxy)
{
	proxy->last_io = ioloop_time;
	if (o_stream_flush(proxy->server_output) < 0) {
		login_proxy_free_errno(&proxy,
				       proxy->server_output->stream_errno,
				       "server");
		return 1;
	}

	if (proxy->client_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->server_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in proxy's output buffer, so we can
		   read more from client. */
		proxy->client_io = io_add(proxy->client_fd, IO_READ,
					  proxy_client_input, proxy);
	}
	return 1;
}

static int proxy_client_output(struct login_proxy *proxy)
{
	proxy->last_io = ioloop_time;
	if (o_stream_flush(proxy->client_output) < 0) {
		login_proxy_free_errno(&proxy,
				       proxy->client_output->stream_errno,
				       "client");
		return 1;
	}

	if (proxy->server_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->client_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in client's output buffer, so we can
		   read more from proxy. */
		proxy->server_io =
			io_add(proxy->server_fd, IO_READ, server_input, proxy);
	}
	return 1;
}

static void proxy_prelogin_input(struct login_proxy *proxy)
{
	proxy->callback(proxy->client);
}

static void proxy_plain_connected(struct login_proxy *proxy)
{
	proxy->server_input =
		i_stream_create_fd(proxy->server_fd, MAX_PROXY_INPUT_SIZE,
				   FALSE);
	proxy->server_output =
		o_stream_create_fd(proxy->server_fd, (size_t)-1, FALSE);
	o_stream_set_no_error_handling(proxy->server_output, TRUE);

	proxy->server_io =
		io_add(proxy->server_fd, IO_READ, proxy_prelogin_input, proxy);
}

static void proxy_fail_connect(struct login_proxy *proxy)
{
	if (timeval_cmp(&proxy->created, &proxy->state_rec->last_success) < 0) {
		/* there was a successful connection done since we started
		   connecting. perhaps this is just a temporary one-off
		   failure. */
	} else {
		proxy->state_rec->last_failure = ioloop_timeval;
	}
	proxy->state_rec->num_waiting_connections--;
	proxy->state_rec = NULL;
}

static void
proxy_log_connect_error(struct login_proxy *proxy)
{
	string_t *str = t_str_new(128);
	struct ip_addr local_ip;
	unsigned int local_port;

	str_printfa(str, "proxy(%s): ", proxy->client->virtual_user);
	if (!proxy->connected) {
		str_printfa(str, "connect(%s, %u) failed: %m",
			    proxy->host, proxy->port);
	} else {
		str_printfa(str, "Login for %s:%u timed out in state=%u",
			    proxy->host, proxy->port,
			    proxy->client->proxy_state);
	}
	str_printfa(str, " (after %u secs",
		    (unsigned int)(ioloop_time - proxy->created.tv_sec));

	if (proxy->server_fd != -1 &&
	    net_getsockname(proxy->server_fd, &local_ip, &local_port) == 0) {
		str_printfa(str, ", local=%s:%u",
			    net_ip2addr(&local_ip), local_port);
	} else if (proxy->source_ip.family != 0) {
		str_printfa(str, ", local=%s",
			    net_ip2addr(&proxy->source_ip));
	}

	str_append_c(str, ')');
	i_error("%s", str_c(str));
}

static void proxy_wait_connect(struct login_proxy *proxy)
{
	errno = net_geterror(proxy->server_fd);
	if (errno != 0) {
		proxy_log_connect_error(proxy);
		proxy_fail_connect(proxy);
                login_proxy_free(&proxy);
		return;
	}
	proxy->connected = TRUE;
	proxy->state_rec->last_success = ioloop_timeval;
	proxy->state_rec->num_waiting_connections--;
	proxy->state_rec = NULL;

	if ((proxy->ssl_flags & PROXY_SSL_FLAG_YES) != 0 &&
	    (proxy->ssl_flags & PROXY_SSL_FLAG_STARTTLS) == 0) {
		if (login_proxy_starttls(proxy) < 0) {
			login_proxy_free(&proxy);
			return;
		}
	} else {
		io_remove(&proxy->server_io);
		proxy_plain_connected(proxy);
	}
}

static void proxy_connect_timeout(struct login_proxy *proxy)
{
	errno = ETIMEDOUT;
	proxy_log_connect_error(proxy);
	if (!proxy->connected)
		proxy_fail_connect(proxy);
	login_proxy_free(&proxy);
}

static int login_proxy_connect(struct login_proxy *proxy)
{
	struct login_proxy_record *rec;

	rec = login_proxy_state_get(proxy_state, &proxy->ip, proxy->port);
	if (timeval_cmp(&rec->last_failure, &rec->last_success) > 0 &&
	    rec->last_failure.tv_sec - rec->last_success.tv_sec > PROXY_IMMEDIATE_FAILURE_SECS &&
	    rec->num_waiting_connections != 0) {
		/* the server is down. fail immediately */
		i_error("proxy(%s): Host %s:%u is down",
			proxy->client->virtual_user, proxy->host, proxy->port);
		login_proxy_free(&proxy);
		return -1;
	}

	proxy->server_fd = net_connect_ip(&proxy->ip, proxy->port,
					  proxy->source_ip.family == 0 ? NULL :
					  &proxy->source_ip);
	if (proxy->server_fd == -1) {
		proxy_log_connect_error(proxy);
		login_proxy_free(&proxy);
		return -1;
	}
	proxy->server_io = io_add(proxy->server_fd, IO_WRITE,
				  proxy_wait_connect, proxy);
	if (proxy->connect_timeout_msecs != 0) {
		proxy->to = timeout_add(proxy->connect_timeout_msecs,
					proxy_connect_timeout, proxy);
	}

	proxy->state_rec = rec;
	proxy->state_rec->num_waiting_connections++;
	return 0;
}

int login_proxy_new(struct client *client,
		    const struct login_proxy_settings *set,
		    proxy_callback_t *callback)
{
	struct login_proxy *proxy;

	i_assert(client->login_proxy == NULL);

	if (set->host == NULL || *set->host == '\0') {
		i_error("proxy(%s): host not given", client->virtual_user);
		return -1;
	}

	if (client->proxy_ttl <= 1) {
		i_error("proxy(%s): TTL reached zero - "
			"proxies appear to be looping?", client->virtual_user);
		return -1;
	}

	proxy = i_new(struct login_proxy, 1);
	proxy->client = client;
	proxy->client_fd = -1;
	proxy->server_fd = -1;
	proxy->created = ioloop_timeval;
	proxy->ip = set->ip;
	proxy->source_ip = set->source_ip;
	proxy->host = i_strdup(set->host);
	proxy->port = set->port;
	proxy->connect_timeout_msecs = set->connect_timeout_msecs;
	proxy->notify_refresh_secs = set->notify_refresh_secs;
	proxy->ssl_flags = set->ssl_flags;
	client_ref(client);

	if (set->ip.family == 0 &&
	    net_addr2ip(set->host, &proxy->ip) < 0) {
		i_error("proxy(%s): BUG: host %s is not an IP "
			"(auth should have changed it)",
			client->virtual_user, set->host);
	} else {
		if (login_proxy_connect(proxy) < 0)
			return -1;
	}

	DLLIST_PREPEND(&login_proxies_pending, proxy);

	proxy->callback = callback;
	client->login_proxy = proxy;
	return 0;
}

static void ATTR_NULL(2)
login_proxy_free_reason(struct login_proxy **_proxy, const char *reason)
{
	struct login_proxy *proxy = *_proxy;
	struct client *client = proxy->client;
	const char *ipstr;

	*_proxy = NULL;

	if (proxy->destroying)
		return;
	proxy->destroying = TRUE;

	if (proxy->to != NULL)
		timeout_remove(&proxy->to);
	if (proxy->to_notify != NULL)
		timeout_remove(&proxy->to_notify);

	if (proxy->state_rec != NULL)
		proxy->state_rec->num_waiting_connections--;
	if (proxy->to != NULL)
		timeout_remove(&proxy->to);

	if (proxy->server_io != NULL)
		io_remove(&proxy->server_io);
	if (proxy->server_input != NULL)
		i_stream_destroy(&proxy->server_input);
	if (proxy->server_output != NULL)
		o_stream_destroy(&proxy->server_output);

	if (proxy->client_fd != -1) {
		/* detached proxy */
		DLLIST_REMOVE(&login_proxies, proxy);

		ipstr = net_ip2addr(&proxy->client->ip);
		client_log(proxy->client, t_strdup_printf(
			"proxy(%s): disconnecting %s%s",
			proxy->client->virtual_user,
			ipstr != NULL ? ipstr : "",
			reason == NULL ? "" : t_strdup_printf(" (%s)", reason)));

		if (proxy->client_io != NULL)
			io_remove(&proxy->client_io);
		if (proxy->client_output != NULL)
			o_stream_destroy(&proxy->client_output);
		net_disconnect(proxy->client_fd);
	} else {
		i_assert(proxy->client_io == NULL);
		i_assert(proxy->client_output == NULL);

		DLLIST_REMOVE(&login_proxies_pending, proxy);

		if (proxy->callback != NULL)
			proxy->callback(proxy->client);
	}

	if (proxy->server_fd != -1)
		net_disconnect(proxy->server_fd);

	if (proxy->ssl_server_proxy != NULL)
		ssl_proxy_free(&proxy->ssl_server_proxy);
	i_free(proxy->host);
	i_free(proxy);

	client->login_proxy = NULL;
	client_unref(&client);
}

void login_proxy_free(struct login_proxy **_proxy)
{
	login_proxy_free_reason(_proxy, NULL);
}

bool login_proxy_is_ourself(const struct client *client, const char *host,
			    unsigned int port, const char *destuser)
{
	struct ip_addr ip;

	if (port != client->local_port)
		return FALSE;

	if (net_addr2ip(host, &ip) < 0)
		return FALSE;
	if (!net_ip_compare(&ip, &client->local_ip))
		return FALSE;

	return strcmp(client->virtual_user, destuser) == 0;
}

struct istream *login_proxy_get_istream(struct login_proxy *proxy)
{
	return proxy->disconnecting ? NULL : proxy->server_input;
}

struct ostream *login_proxy_get_ostream(struct login_proxy *proxy)
{
	return proxy->server_output;
}

const char *login_proxy_get_host(const struct login_proxy *proxy)
{
	return proxy->host;
}

unsigned int login_proxy_get_port(const struct login_proxy *proxy)
{
	return proxy->port;
}

enum login_proxy_ssl_flags
login_proxy_get_ssl_flags(const struct login_proxy *proxy)
{
	return proxy->ssl_flags;
}

static void login_proxy_notify(struct login_proxy *proxy)
{
	login_proxy_state_notify(proxy_state, proxy->client->proxy_user);
}

void login_proxy_detach(struct login_proxy *proxy)
{
	struct client *client = proxy->client;
	const unsigned char *data;
	size_t size;

	i_assert(proxy->client_fd == -1);
	i_assert(proxy->server_output != NULL);

	if (proxy->to != NULL)
		timeout_remove(&proxy->to);

	proxy->client_fd = i_stream_get_fd(client->input);
	proxy->client_output = client->output;

	o_stream_set_max_buffer_size(client->output, (size_t)-1);
	o_stream_set_flush_callback(client->output, proxy_client_output, proxy);
	client->output = NULL;

	/* send all pending client input to proxy and get rid of the stream */
	data = i_stream_get_data(client->input, &size);
	if (size != 0)
		o_stream_nsend(proxy->server_output, data, size);

	/* from now on, just do dummy proxying */
	io_remove(&proxy->server_io);
	proxy->server_io =
		io_add(proxy->server_fd, IO_READ, server_input, proxy);
	proxy->client_io =
		io_add(proxy->client_fd, IO_READ, proxy_client_input, proxy);
	o_stream_set_flush_callback(proxy->server_output, server_output, proxy);
	i_stream_destroy(&proxy->server_input);

	if (proxy->notify_refresh_secs != 0) {
		proxy->to_notify =
			timeout_add(proxy->notify_refresh_secs * 1000,
				    login_proxy_notify, proxy);
	}

	proxy->callback = NULL;

	if (login_proxy_ipc_server == NULL) {
		login_proxy_ipc_server =
			ipc_server_init(LOGIN_PROXY_IPC_PATH,
					LOGIN_PROXY_IPC_NAME,
					login_proxy_ipc_cmd);
	}

	DLLIST_REMOVE(&login_proxies_pending, proxy);
	DLLIST_PREPEND(&login_proxies, proxy);

	client->fd = -1;
	client->login_proxy = NULL;
}

static int login_proxy_ssl_handshaked(void *context)
{
	struct login_proxy *proxy = context;

	if ((proxy->ssl_flags & PROXY_SSL_FLAG_ANY_CERT) != 0)
		return 0;

	if (ssl_proxy_has_broken_client_cert(proxy->ssl_server_proxy)) {
		client_log_err(proxy->client, t_strdup_printf(
			"proxy: Received invalid SSL certificate from %s:%u: %s",
			proxy->host, proxy->port,
			ssl_proxy_get_cert_error(proxy->ssl_server_proxy)));
	} else if (!ssl_proxy_has_valid_client_cert(proxy->ssl_server_proxy)) {
		client_log_err(proxy->client, t_strdup_printf(
			"proxy: SSL certificate not received from %s:%u",
			proxy->host, proxy->port));
	} else if (ssl_proxy_cert_match_name(proxy->ssl_server_proxy,
					     proxy->host) < 0) {
		client_log_err(proxy->client, t_strdup_printf(
			"proxy: hostname doesn't match SSL certificate at %s:%u",
			proxy->host, proxy->port));
	} else {
		return 0;
	}
	proxy->disconnecting = TRUE;
	return -1;
}

int login_proxy_starttls(struct login_proxy *proxy)
{
	int fd;

	if (proxy->server_input != NULL)
		i_stream_destroy(&proxy->server_input);
	if (proxy->server_output != NULL)
		o_stream_destroy(&proxy->server_output);
	io_remove(&proxy->server_io);

	fd = ssl_proxy_client_alloc(proxy->server_fd, &proxy->client->ip,
				    proxy->client->pool, proxy->client->set,
				    proxy->client->ssl_set,
				    login_proxy_ssl_handshaked, proxy,
				    &proxy->ssl_server_proxy);
	if (fd < 0) {
		client_log_err(proxy->client, t_strdup_printf(
			"proxy: SSL handshake failed to %s:%u",
			proxy->host, proxy->port));
		return -1;
	}
	ssl_proxy_set_client(proxy->ssl_server_proxy, proxy->client);
	ssl_proxy_start(proxy->ssl_server_proxy);

	proxy->server_fd = fd;
	proxy_plain_connected(proxy);
	return 0;
}

static void proxy_kill_idle(struct login_proxy *proxy)
{
	login_proxy_free_reason(&proxy, KILLED_BY_ADMIN_REASON);
}

void login_proxy_kill_idle(void)
{
	struct login_proxy *proxy, *next;
	time_t now = time(NULL);
	time_t stop_timestamp = now - LOGIN_PROXY_DIE_IDLE_SECS;
	unsigned int stop_msecs;

	for (proxy = login_proxies; proxy != NULL; proxy = next) {
		next = proxy->next;

		if (proxy->last_io <= stop_timestamp)
			proxy_kill_idle(proxy);
		else {
			i_assert(proxy->to == NULL);
			stop_msecs = (proxy->last_io - stop_timestamp) * 1000;
			proxy->to = timeout_add(stop_msecs,
						proxy_kill_idle, proxy);
		}
	}
}

static void
login_proxy_cmd_kick(struct ipc_cmd *cmd, const char *const *args)
{
	struct login_proxy *proxy, *next;
	unsigned int count = 0;

	if (args[0] == NULL) {
		ipc_cmd_fail(&cmd, "Missing parameter");
		return;
	}

	for (proxy = login_proxies; proxy != NULL; proxy = next) {
		next = proxy->next;

		if (strcmp(proxy->client->virtual_user, args[0]) == 0) {
			login_proxy_free_reason(&proxy, KILLED_BY_ADMIN_REASON);
			count++;
		}
	}
	for (proxy = login_proxies_pending; proxy != NULL; proxy = next) {
		next = proxy->next;

		if (strcmp(proxy->client->virtual_user, args[0]) == 0) {
			client_destroy(proxy->client, "Connection kicked");
			count++;
		}
	}
	ipc_cmd_success_reply(&cmd, t_strdup_printf("%u", count));
}

static unsigned int director_username_hash(struct client *client)
{
	return mail_user_hash(client->virtual_user,
			      client->set->director_username_hash);
}

static void
login_proxy_cmd_kick_director_hash(struct ipc_cmd *cmd, const char *const *args)
{
	struct login_proxy *proxy, *next;
	struct ip_addr except_ip;
	unsigned int hash, count = 0;

	if (args[0] == NULL || str_to_uint(args[0], &hash) < 0) {
		ipc_cmd_fail(&cmd, "Invalid parameters");
		return;
	}
	/* optional except_ip parameter specifies that we're not killing the
	   connections that are proxying to the except_ip backend */
	except_ip.family = 0;
	if (args[1] != NULL && args[1][0] != '\0' &&
	    net_addr2ip(args[1], &except_ip) < 0) {
		ipc_cmd_fail(&cmd, "Invalid except_ip parameter");
		return;
	}

	for (proxy = login_proxies; proxy != NULL; proxy = next) {
		next = proxy->next;

		if (director_username_hash(proxy->client) == hash &&
		    !net_ip_compare(&proxy->ip, &except_ip)) {
			login_proxy_free_reason(&proxy, KILLED_BY_ADMIN_REASON);
			count++;
		}
	}
	for (proxy = login_proxies_pending; proxy != NULL; proxy = next) {
		next = proxy->next;

		if (director_username_hash(proxy->client) == hash &&
		    !net_ip_compare(&proxy->ip, &except_ip)) {
			client_destroy(proxy->client, "Connection kicked");
			count++;
		}
	}
	ipc_cmd_success_reply(&cmd, t_strdup_printf("%u", count));
}

static void
login_proxy_cmd_list_reply(struct ipc_cmd *cmd,
			   struct login_proxy *proxy)
{
	T_BEGIN {
		const char *reply;

		reply = t_strdup_printf("%s\t%s\t%s\t%s\t%u",
					proxy->client->virtual_user,
					login_binary->protocol,
					net_ip2addr(&proxy->client->ip),
					net_ip2addr(&proxy->ip), proxy->port);
		ipc_cmd_send(cmd, reply);
	} T_END;
}

static void
login_proxy_cmd_list(struct ipc_cmd *cmd, const char *const *args ATTR_UNUSED)
{
	struct login_proxy *proxy;

	for (proxy = login_proxies; proxy != NULL; proxy = proxy->next)
		login_proxy_cmd_list_reply(cmd, proxy);
	for (proxy = login_proxies_pending; proxy != NULL; proxy = proxy->next)
		login_proxy_cmd_list_reply(cmd, proxy);
	ipc_cmd_success(&cmd);
}

static void login_proxy_ipc_cmd(struct ipc_cmd *cmd, const char *line)
{
	const char *const *args = t_strsplit_tab(line);
	const char *name = args[0];

	args++;
	if (strcmp(name, "KICK") == 0)
		login_proxy_cmd_kick(cmd, args);
	else if (strcmp(name, "KICK-DIRECTOR-HASH") == 0)
		login_proxy_cmd_kick_director_hash(cmd, args);
	else if (strcmp(name, "LIST") == 0)
		login_proxy_cmd_list(cmd, args);
	else
		ipc_cmd_fail(&cmd, "Unknown command");
}

void login_proxy_init(const char *proxy_notify_pipe_path)
{
	proxy_state = login_proxy_state_init(proxy_notify_pipe_path);
}

void login_proxy_deinit(void)
{
	struct login_proxy *proxy;

	while (login_proxies != NULL) {
		proxy = login_proxies;
		login_proxy_free_reason(&proxy, KILLED_BY_ADMIN_REASON);
	}
	if (login_proxy_ipc_server != NULL)
		ipc_server_deinit(&login_proxy_ipc_server);
	login_proxy_state_deinit(&proxy_state);
}
