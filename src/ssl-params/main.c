/* Copyright (c) 2009-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "array.h"
#include "ostream.h"
#include "restrict-access.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "ssl-params-settings.h"
#include "ssl-params.h"

#include <sys/wait.h>

#define SSL_BUILD_PARAM_FNAME "ssl-parameters.dat"
#define STARTUP_IDLE_TIMEOUT_MSECS 1000

struct client {
	int fd;
	struct ostream *output;
};

static ARRAY(int) delayed_fds;
static struct ssl_params *param;
static buffer_t *ssl_params;
static struct timeout *to_startup;

static void client_deinit(struct ostream *output)
{
	o_stream_destroy(&output);
	master_service_client_connection_destroyed(master_service);
}

static int client_output_flush(struct ostream *output)
{
	if (o_stream_flush(output) == 0) {
		/* more to come */
		return 0;
	}
	/* finished / disconnected */
	client_deinit(output);
	return -1;
}

static void client_handle(int fd)
{
	struct ostream *output;

	output = o_stream_create_fd_autoclose(&fd, (size_t)-1);
	if (o_stream_send(output, ssl_params->data, ssl_params->used) < 0 ||
	    o_stream_get_buffer_used_size(output) == 0)
		client_deinit(output);
	else {
		o_stream_set_flush_callback(output, client_output_flush,
					    output);
	}
}

static void client_connected(struct master_service_connection *conn)
{
	if (to_startup != NULL)
		timeout_remove(&to_startup);
	master_service_client_connection_accept(conn);
	if (ssl_params->used == 0) {
		/* waiting for parameter building to finish */
		if (!array_is_created(&delayed_fds))
			i_array_init(&delayed_fds, 32);
		array_append(&delayed_fds, &conn->fd, 1);
	} else {
		client_handle(conn->fd);
	}
}

static void ssl_params_callback(const unsigned char *data, size_t size)
{
	const int *fds;

	buffer_set_used_size(ssl_params, 0);
	buffer_append(ssl_params, data, size);

	if (!array_is_created(&delayed_fds)) {
		/* if we don't get client connections soon, it means master
		   ran us at startup to make sure ssl parameters are generated
		   asap. if we're here because of that, don't bother hanging
		   around to see if we get any client connections. */
		if (to_startup == NULL) {
			to_startup = timeout_add(STARTUP_IDLE_TIMEOUT_MSECS,
						 master_service_stop,
						 master_service);
		}
		return;
	}

	array_foreach(&delayed_fds, fds)
		client_handle(*fds);
	array_free(&delayed_fds);
}

static void sig_chld(const siginfo_t *si ATTR_UNUSED, void *context ATTR_UNUSED)
{
	int status;

	if (waitpid(-1, &status, WNOHANG) < 0)
		i_error("waitpid() failed: %m");
	else if (status != 0)
		i_error("child process failed with status %d", status);
	else {
		/* params should have been created now. try refreshing. */
		ssl_params_refresh(param);
	}
}

static void main_init(const struct ssl_params_settings *set)
{
	const struct master_service_settings *service_set;
	const char *filename;

	lib_signals_set_handler(SIGCHLD, LIBSIG_FLAGS_SAFE, sig_chld, NULL);

	ssl_params = buffer_create_dynamic(default_pool, 1024);
	service_set = master_service_settings_get(master_service);
	filename = t_strconcat(service_set->state_dir,
			       "/"SSL_BUILD_PARAM_FNAME, NULL);
	param = ssl_params_init(filename, ssl_params_callback, set);
}

static void main_deinit(void)
{
	ssl_params_deinit(&param);
	if (to_startup != NULL)
		timeout_remove(&to_startup);
	if (array_is_created(&delayed_fds))
		array_free(&delayed_fds);
}

int main(int argc, char *argv[])
{
	const struct ssl_params_settings *set;

	master_service = master_service_init("ssl-params", 0, &argc, &argv, "");
	master_service_init_log(master_service, "ssl-params: ");

	if (master_getopt(master_service) > 0)
		return FATAL_DEFAULT;
	set = ssl_params_settings_read(master_service);

	restrict_access_by_env(NULL, FALSE);
	restrict_access_allow_coredumps(TRUE);

#ifndef HAVE_SSL
	i_fatal("Dovecot built without SSL support");
#endif

	main_init(set);
	master_service_init_finish(master_service);
	master_service_run(master_service, client_connected);
	main_deinit();

	master_service_deinit(&master_service);
        return 0;
}
