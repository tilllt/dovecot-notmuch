/* Copyright (c) 2002-2014 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "array.h"
#include "ioloop.h"
#include "net.h"
#include "ostream.h"
#include "read-full.h"
#include "safe-memset.h"
#include "hash.h"
#include "llist.h"
#include "master-interface.h"
#include "master-service-ssl-settings.h"
#include "client-common.h"
#include "ssl-proxy.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef HAVE_OPENSSL

#include "iostream-openssl.h"
#include <openssl/crypto.h>
#include <openssl/engine.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#if !defined(OPENSSL_NO_ECDH) && OPENSSL_VERSION_NUMBER >= 0x10000000L
#  define HAVE_ECDH
#endif

/* Check every 30 minutes if parameters file has been updated */
#define SSL_PARAMFILE_CHECK_INTERVAL (60*30)

#define SSL_PARAMETERS_PATH "ssl-params"

#ifndef SSL_CTRL_SET_TLSEXT_HOSTNAME /* FIXME: this may be unnecessary.. */
#  undef HAVE_SSL_GET_SERVERNAME
#endif

enum ssl_io_action {
	SSL_ADD_INPUT,
	SSL_REMOVE_INPUT,
	SSL_ADD_OUTPUT,
	SSL_REMOVE_OUTPUT
};

struct ssl_proxy {
	int refcount;
	struct ssl_proxy *prev, *next;

	SSL *ssl;
	struct client *client;
	struct ip_addr ip;
	const struct login_settings *login_set;
	const struct master_service_ssl_settings *ssl_set;
	pool_t set_pool;

	int fd_ssl, fd_plain;
	struct io *io_ssl_read, *io_ssl_write, *io_plain_read, *io_plain_write;

	unsigned char plainout_buf[1024];
	unsigned int plainout_size;

	unsigned char sslout_buf[1024];
	unsigned int sslout_size;

	ssl_handshake_callback_t *handshake_callback;
	void *handshake_context;

	const char *cert_error;
	char *last_error;
	unsigned int handshaked:1;
	unsigned int destroyed:1;
	unsigned int cert_received:1;
	unsigned int cert_broken:1;
	unsigned int client_proxy:1;
	unsigned int flushing:1;
};

struct ssl_parameters {
	const char *path;
	time_t last_refresh;
	int fd;

	DH *dh_512, *dh_default;
};

struct ssl_server_context {
	SSL_CTX *ctx;
	pool_t pool;

	const char *cert;
	const char *key;
	const char *ca;
	const char *cipher_list;
	const char *protocols;
	bool verify_client_cert;
	bool prefer_server_ciphers;
	bool compression;
};

static int extdata_index;
static HASH_TABLE(struct ssl_server_context *,
		  struct ssl_server_context *) ssl_servers;
static SSL_CTX *ssl_client_ctx;
static unsigned int ssl_proxy_count;
static struct ssl_proxy *ssl_proxies;
static struct ssl_parameters ssl_params;
static int ssl_username_nid;
static ENGINE *ssl_engine;

static void plain_read(struct ssl_proxy *proxy);
static void ssl_read(struct ssl_proxy *proxy);
static void ssl_write(struct ssl_proxy *proxy);
static void ssl_step(struct ssl_proxy *proxy);
static void ssl_proxy_unref(struct ssl_proxy *proxy);

static struct ssl_server_context *
ssl_server_context_init(const struct login_settings *login_set,
			const struct master_service_ssl_settings *ssl_set);
static void ssl_server_context_deinit(struct ssl_server_context **_ctx);

static void ssl_proxy_ctx_set_crypto_params(SSL_CTX *ssl_ctx,
                                            const struct master_service_ssl_settings *set);
#if defined(HAVE_ECDH) && !defined(SSL_CTRL_SET_ECDH_AUTO)
static int ssl_proxy_ctx_get_pkey_ec_curve_name(const struct master_service_ssl_settings *set);
#endif

static unsigned int ssl_server_context_hash(const struct ssl_server_context *ctx)
{
	unsigned int i, g, h = 0;

	/* checking for different certs is typically good enough,
	   and it should be enough to check only the first few bytes. */
	for (i = 0; i < 16 && ctx->cert[i] != '\0'; i++) {
		h = (h << 4) + ctx->cert[i];
		if ((g = h & 0xf0000000UL)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
	}
	return h;
}

static int ssl_server_context_cmp(const struct ssl_server_context *ctx1,
				  const struct ssl_server_context *ctx2)
{
	if (strcmp(ctx1->cert, ctx2->cert) != 0)
		return 1;
	if (strcmp(ctx1->key, ctx2->key) != 0)
		return 1;
	if (null_strcmp(ctx1->ca, ctx2->ca) != 0)
		return 1;
	if (null_strcmp(ctx1->cipher_list, ctx2->cipher_list) != 0)
		return 1;
	if (null_strcmp(ctx1->protocols, ctx2->protocols) != 0)
		return 1;

	return ctx1->verify_client_cert == ctx2->verify_client_cert ? 0 : 1;
}

static void ssl_params_corrupted(const char *reason)
{
	i_fatal("Corrupted SSL ssl-parameters.dat in state_dir: %s", reason);
}

static void read_next(struct ssl_parameters *params, void *data, size_t size)
{
	int ret;

	if ((ret = read_full(params->fd, data, size)) < 0)
		i_fatal("read(%s) failed: %m", params->path);
	if (ret == 0)
		ssl_params_corrupted("Truncated file");
}

static bool read_dh_parameters_next(struct ssl_parameters *params)
{
	unsigned char *buf;
	const unsigned char *cbuf;
	unsigned int len;
	int bits;

	/* read bit size. 0 ends the DH parameters list. */
	read_next(params, &bits, sizeof(bits));

	if (bits == 0)
		return FALSE;

	/* read data size. */
	read_next(params, &len, sizeof(len));
	if (len > 1024*100) /* should be enough? */
		ssl_params_corrupted("File too large");

	buf = i_malloc(len);
	read_next(params, buf, len);

	cbuf = buf;
	switch (bits) {
	case 512:
		if (params->dh_512 != NULL)
			ssl_params_corrupted("Duplicate 512bit parameters");
		params->dh_512 = d2i_DHparams(NULL, &cbuf, len);
		break;
	default:
		if (params->dh_default != NULL)
			ssl_params_corrupted("Duplicate default parameters");
		params->dh_default = d2i_DHparams(NULL, &cbuf, len);
		break;
	}

	i_free(buf);
	return TRUE;
}

static void ssl_free_parameters(struct ssl_parameters *params)
{
	if (params->dh_512 != NULL) {
		DH_free(params->dh_512);
                params->dh_512 = NULL;
	}
	if (params->dh_default != NULL) {
		DH_free(params->dh_default);
                params->dh_default = NULL;
	}
}

static void ssl_refresh_parameters(struct ssl_parameters *params)
{
	char c;
	int ret;

	if (params->last_refresh > ioloop_time - SSL_PARAMFILE_CHECK_INTERVAL)
		return;
	params->last_refresh = ioloop_time;

	params->fd = net_connect_unix(params->path);
	if (params->fd == -1) {
		i_error("connect(%s) failed: %m", params->path);
		return;
	}
	net_set_nonblock(params->fd, FALSE);

	ssl_free_parameters(params);
	while (read_dh_parameters_next(params)) ;

	if ((ret = read_full(params->fd, &c, 1)) < 0)
		i_fatal("read(%s) failed: %m", params->path);
	else if (ret != 0) {
		/* more data than expected */
		ssl_params_corrupted("More data than expected");
	}

	if (close(params->fd) < 0)
		i_error("close(%s) failed: %m", params->path);
	params->fd = -1;
}

static void ssl_set_io(struct ssl_proxy *proxy, enum ssl_io_action action)
{
	switch (action) {
	case SSL_ADD_INPUT:
		if (proxy->io_ssl_read != NULL)
			break;
		proxy->io_ssl_read = io_add(proxy->fd_ssl, IO_READ,
					    ssl_step, proxy);
		break;
	case SSL_REMOVE_INPUT:
		if (proxy->io_ssl_read != NULL)
			io_remove(&proxy->io_ssl_read);
		break;
	case SSL_ADD_OUTPUT:
		if (proxy->io_ssl_write != NULL)
			break;
		proxy->io_ssl_write = io_add(proxy->fd_ssl, IO_WRITE,
					     ssl_step, proxy);
		break;
	case SSL_REMOVE_OUTPUT:
		if (proxy->io_ssl_write != NULL)
			io_remove(&proxy->io_ssl_write);
		break;
	}
}

static void plain_block_input(struct ssl_proxy *proxy, bool block)
{
	if (block) {
		if (proxy->io_plain_read != NULL)
			io_remove(&proxy->io_plain_read);
	} else {
		if (proxy->io_plain_read == NULL) {
			proxy->io_plain_read = io_add(proxy->fd_plain, IO_READ,
						      plain_read, proxy);
		}
	}
}

static void plain_read(struct ssl_proxy *proxy)
{
	ssize_t ret;
	bool corked = FALSE;

	if (proxy->sslout_size == sizeof(proxy->sslout_buf)) {
		/* buffer full, block input until it's written */
		plain_block_input(proxy, TRUE);
		return;
	}

	proxy->refcount++;

	while (proxy->sslout_size < sizeof(proxy->sslout_buf) &&
	       !proxy->destroyed) {
		ret = net_receive(proxy->fd_plain,
				  proxy->sslout_buf + proxy->sslout_size,
				  sizeof(proxy->sslout_buf) -
				  proxy->sslout_size);
		if (ret <= 0) {
			if (ret < 0)
				ssl_proxy_destroy(proxy);
			break;
		} else {
			proxy->sslout_size += ret;
			if (!corked) {
				if (net_set_cork(proxy->fd_ssl, TRUE) == 0)
					corked = TRUE;
			}
			ssl_write(proxy);
		}
	}

	if (corked)
		(void)net_set_cork(proxy->fd_ssl, FALSE);

	ssl_proxy_unref(proxy);
}

static void plain_write(struct ssl_proxy *proxy)
{
	ssize_t ret;

	proxy->refcount++;

	ret = net_transmit(proxy->fd_plain, proxy->plainout_buf,
			   proxy->plainout_size);
	if (ret < 0)
		ssl_proxy_destroy(proxy);
	else {
		proxy->plainout_size -= ret;
		memmove(proxy->plainout_buf, proxy->plainout_buf + ret,
			proxy->plainout_size);

		if (proxy->plainout_size > 0) {
			if (proxy->io_plain_write == NULL) {
				proxy->io_plain_write =
					io_add(proxy->fd_plain, IO_WRITE,
					       plain_write, proxy);
			}
		} else {
			if (proxy->io_plain_write != NULL)
				io_remove(&proxy->io_plain_write);
		}

		ssl_set_io(proxy, SSL_ADD_INPUT);
		if (SSL_pending(proxy->ssl) > 0)
			ssl_read(proxy);
	}

	ssl_proxy_unref(proxy);
}

static const char *ssl_err2str(unsigned long err, const char *data, int flags)
{
	const char *ret;
	char *buf;
	size_t err_size = 256;

	buf = t_malloc(err_size);
	buf[err_size-1] = '\0';
	ERR_error_string_n(err, buf, err_size-1);
	ret = buf;

	if ((flags & ERR_TXT_STRING) != 0)
		ret = t_strdup_printf("%s: %s", buf, data);
	return ret;
}

static const char *ssl_last_error(void)
{
	unsigned long err;
	const char *data;
	int flags;

	err = ERR_get_error_line_data(NULL, NULL, &data, &flags);
	while (err != 0 && ERR_peek_error() != 0) {
		i_error("SSL: Stacked error: %s",
			ssl_err2str(err, data, flags));
		err = ERR_get_error();
	}
	if (err == 0) {
		if (errno != 0)
			return strerror(errno);
		return "Unknown error";
	}
	return ssl_err2str(err, data, flags);
}

static void ssl_handle_error(struct ssl_proxy *proxy, int ret,
			     const char *func_name)
{
	const char *errstr = NULL;
	int err;

	proxy->refcount++;

	i_free_and_null(proxy->last_error);
	err = SSL_get_error(proxy->ssl, ret);

	switch (err) {
	case SSL_ERROR_WANT_READ:
		ssl_set_io(proxy, SSL_ADD_INPUT);
		break;
	case SSL_ERROR_WANT_WRITE:
		ssl_set_io(proxy, SSL_ADD_OUTPUT);
		break;
	case SSL_ERROR_SYSCALL:
		/* eat up the error queue */
		if (ERR_peek_error() != 0)
			errstr = ssl_last_error();
		else if (ret != 0)
			errstr = strerror(errno);
		else {
			/* EOF. */
			errstr = "Disconnected";
			break;
		}
		errstr = t_strdup_printf("%s syscall failed: %s",
					 func_name, errstr);
		break;
	case SSL_ERROR_ZERO_RETURN:
		/* clean connection closing */
		ssl_proxy_destroy(proxy);
		break;
	case SSL_ERROR_SSL:
		if (ERR_GET_REASON(ERR_peek_error()) == ERR_R_MALLOC_FAILURE) {
			i_error("OpenSSL malloc() failed. "
				"You may need to increase service %s { vsz_limit }",
				login_binary->process_name);
		}
		errstr = t_strdup_printf("%s failed: %s",
					 func_name, ssl_last_error());
		break;
	default:
		errstr = t_strdup_printf("%s failed: unknown failure %d (%s)",
					 func_name, err, ssl_last_error());
		break;
	}

	if (errstr != NULL) {
		proxy->last_error = i_strdup(errstr);
		ssl_proxy_destroy(proxy);
	}
	ssl_proxy_unref(proxy);
}

static void ssl_handshake(struct ssl_proxy *proxy)
{
	int ret;

	if (proxy->client_proxy) {
		ret = SSL_connect(proxy->ssl);
		if (ret != 1) {
			ssl_handle_error(proxy, ret, "SSL_connect()");
			return;
		}
	} else {
		ret = SSL_accept(proxy->ssl);
		if (ret != 1) {
			ssl_handle_error(proxy, ret, "SSL_accept()");
			return;
		}
	}
	i_free_and_null(proxy->last_error);
	proxy->handshaked = TRUE;

	ssl_set_io(proxy, SSL_ADD_INPUT);
	plain_block_input(proxy, FALSE);

	if (proxy->handshake_callback != NULL) {
		if (proxy->handshake_callback(proxy->handshake_context) < 0)
			ssl_proxy_destroy(proxy);
	}
}

static void ssl_read(struct ssl_proxy *proxy)
{
	int ret;

	while (proxy->plainout_size < sizeof(proxy->plainout_buf) &&
	       !proxy->destroyed) {
		ret = SSL_read(proxy->ssl,
			       proxy->plainout_buf + proxy->plainout_size,
			       sizeof(proxy->plainout_buf) -
			       proxy->plainout_size);
		if (ret <= 0) {
			ssl_handle_error(proxy, ret, "SSL_read()");
			break;
		} else {
			i_free_and_null(proxy->last_error);
			proxy->plainout_size += ret;
			plain_write(proxy);
		}
	}
}

static void ssl_write(struct ssl_proxy *proxy)
{
	int ret;

	ret = SSL_write(proxy->ssl, proxy->sslout_buf, proxy->sslout_size);
	if (ret <= 0)
		ssl_handle_error(proxy, ret, "SSL_write()");
	else {
		i_free_and_null(proxy->last_error);
		proxy->sslout_size -= ret;
		memmove(proxy->sslout_buf, proxy->sslout_buf + ret,
			proxy->sslout_size);

		ssl_set_io(proxy, proxy->sslout_size > 0 ?
			   SSL_ADD_OUTPUT : SSL_REMOVE_OUTPUT);
		plain_block_input(proxy, FALSE);
	}
}

static void ssl_step(struct ssl_proxy *proxy)
{
	proxy->refcount++;

	if (!proxy->handshaked) {
		ssl_set_io(proxy, SSL_REMOVE_OUTPUT);
		ssl_handshake(proxy);
	}

	if (proxy->handshaked) {
		if (proxy->plainout_size == sizeof(proxy->plainout_buf))
			ssl_set_io(proxy, SSL_REMOVE_INPUT);
		else
			ssl_read(proxy);

		if (proxy->sslout_size == 0)
			ssl_set_io(proxy, SSL_REMOVE_OUTPUT);
		else {
			(void)net_set_cork(proxy->fd_ssl, TRUE);
			ssl_write(proxy);
			(void)net_set_cork(proxy->fd_ssl, FALSE);
		}
	}

	ssl_proxy_unref(proxy);
}

static int
ssl_proxy_alloc_common(SSL_CTX *ssl_ctx, int fd, const struct ip_addr *ip,
		       pool_t set_pool, const struct login_settings *login_set,
		       const struct master_service_ssl_settings *ssl_set,
		       struct ssl_proxy **proxy_r)
{
	struct ssl_proxy *proxy;
	SSL *ssl;
	int sfd[2];

	i_assert(fd != -1);

	*proxy_r = NULL;

	if (!ssl_initialized) {
		i_error("SSL support not enabled in configuration");
		return -1;
	}

	ssl_refresh_parameters(&ssl_params);

	ssl = SSL_new(ssl_ctx);
	if (ssl == NULL) {
		i_error("SSL_new() failed: %s", ssl_last_error());
		return -1;
	}

	if (SSL_set_fd(ssl, fd) != 1) {
		i_error("SSL_set_fd() failed: %s", ssl_last_error());
		SSL_free(ssl);
		return -1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sfd) < 0) {
		i_error("socketpair() failed: %m");
		SSL_free(ssl);
		return -1;
	}

	net_set_nonblock(sfd[0], TRUE);
	net_set_nonblock(sfd[1], TRUE);
	net_set_nonblock(fd, TRUE);

	proxy = i_new(struct ssl_proxy, 1);
	proxy->refcount = 2;
	proxy->ssl = ssl;
	proxy->login_set = login_set;
	proxy->ssl_set = ssl_set;
	proxy->fd_ssl = fd;
	proxy->fd_plain = sfd[0];
	proxy->ip = *ip;
	proxy->set_pool = set_pool;
	pool_ref(set_pool);
	SSL_set_ex_data(ssl, extdata_index, proxy);

	ssl_proxy_count++;
	DLLIST_PREPEND(&ssl_proxies, proxy);

	*proxy_r = proxy;
	return sfd[1];
}

static struct ssl_server_context *
ssl_server_context_get(const struct login_settings *login_set,
		       const struct master_service_ssl_settings *set)
{
	struct ssl_server_context *ctx, lookup_ctx;

	memset(&lookup_ctx, 0, sizeof(lookup_ctx));
	lookup_ctx.cert = set->ssl_cert;
	lookup_ctx.key = set->ssl_key;
	lookup_ctx.ca = set->ssl_ca;
	lookup_ctx.cipher_list = set->ssl_cipher_list;
	lookup_ctx.protocols = set->ssl_protocols;
	lookup_ctx.verify_client_cert = set->ssl_verify_client_cert ||
		login_set->auth_ssl_require_client_cert ||
		login_set->auth_ssl_username_from_cert;
	lookup_ctx.prefer_server_ciphers = set->ssl_prefer_server_ciphers;
	lookup_ctx.compression = set->parsed_opts.compression;

	ctx = hash_table_lookup(ssl_servers, &lookup_ctx);
	if (ctx == NULL)
		ctx = ssl_server_context_init(login_set, set);
	return ctx;
}

int ssl_proxy_alloc(int fd, const struct ip_addr *ip, pool_t set_pool,
		    const struct login_settings *login_set,
		    const struct master_service_ssl_settings *ssl_set,
		    struct ssl_proxy **proxy_r)
{
	struct ssl_server_context *ctx;

	ctx = ssl_server_context_get(login_set, ssl_set);
	return ssl_proxy_alloc_common(ctx->ctx, fd, ip,
				      set_pool, login_set, ssl_set, proxy_r);
}

int ssl_proxy_client_alloc(int fd, struct ip_addr *ip, pool_t set_pool,
			   const struct login_settings *login_set,
			   const struct master_service_ssl_settings *ssl_set,
			   ssl_handshake_callback_t *callback, void *context,
			   struct ssl_proxy **proxy_r)
{
	int ret;

	ret = ssl_proxy_alloc_common(ssl_client_ctx, fd, ip,
				     set_pool, login_set, ssl_set, proxy_r);
	if (ret < 0)
		return -1;

	(*proxy_r)->handshake_callback = callback;
	(*proxy_r)->handshake_context = context;
	(*proxy_r)->client_proxy = TRUE;
	return ret;
}

void ssl_proxy_start(struct ssl_proxy *proxy)
{
	ssl_step(proxy);
}

void ssl_proxy_set_client(struct ssl_proxy *proxy, struct client *client)
{
	i_assert(proxy->client == NULL);

	client_ref(client);
	proxy->client = client;
}

bool ssl_proxy_has_valid_client_cert(const struct ssl_proxy *proxy)
{
	return proxy->cert_received && !proxy->cert_broken;
}

bool ssl_proxy_has_broken_client_cert(struct ssl_proxy *proxy)
{
	return proxy->cert_received && proxy->cert_broken;
}

int ssl_proxy_cert_match_name(struct ssl_proxy *proxy, const char *verify_name)
{
	return openssl_cert_match_name(proxy->ssl, verify_name);
}

const char *ssl_proxy_get_peer_name(struct ssl_proxy *proxy)
{
	X509 *x509;
	char *name;
	int len;

	if (!ssl_proxy_has_valid_client_cert(proxy))
		return NULL;

	x509 = SSL_get_peer_certificate(proxy->ssl);
	if (x509 == NULL)
		return NULL; /* we should have had it.. */

	len = X509_NAME_get_text_by_NID(X509_get_subject_name(x509),
					ssl_username_nid, NULL, 0);
	if (len < 0)
		name = "";
	else {
		name = t_malloc(len + 1);
		if (X509_NAME_get_text_by_NID(X509_get_subject_name(x509),
					ssl_username_nid, name, len + 1) < 0)
			name = "";
		else if (strlen(name) != (size_t)len) {
			/* NUL characters in name. Someone's trying to fake
			   being another user? Don't allow it. */
			name = "";
		}
	}
	X509_free(x509);
	
	return *name == '\0' ? NULL : name;
}

bool ssl_proxy_is_handshaked(const struct ssl_proxy *proxy)
{
	return proxy->handshaked;
}

const char *ssl_proxy_get_last_error(const struct ssl_proxy *proxy)
{
	return proxy->last_error;
}

const char *ssl_proxy_get_security_string(struct ssl_proxy *proxy)
{
	const SSL_CIPHER *cipher;
	int bits, alg_bits;
	const char *comp_str;

	if (!proxy->handshaked)
		return "";

	cipher = SSL_get_current_cipher(proxy->ssl);
	bits = SSL_CIPHER_get_bits(cipher, &alg_bits);
	comp_str = ssl_proxy_get_compression(proxy);
	comp_str = comp_str == NULL ? "" : t_strconcat(" ", comp_str, NULL);
	return t_strdup_printf("%s with cipher %s (%d/%d bits)%s",
			       SSL_get_version(proxy->ssl),
			       SSL_CIPHER_get_name(cipher),
			       bits, alg_bits, comp_str);
}

const char *ssl_proxy_get_compression(struct ssl_proxy *proxy ATTR_UNUSED)
{
#if defined(HAVE_SSL_COMPRESSION) && !defined(OPENSSL_NO_COMP)
	const COMP_METHOD *comp;

	comp = SSL_get_current_compression(proxy->ssl);
	return comp == NULL ? NULL : SSL_COMP_get_name(comp);
#else
	return NULL;
#endif
}

const char *ssl_proxy_get_cert_error(struct ssl_proxy *proxy)
{
	return proxy->cert_error != NULL ? proxy->cert_error :
		"(Unknown error)";
}

void ssl_proxy_free(struct ssl_proxy **_proxy)
{
	struct ssl_proxy *proxy = *_proxy;

	*_proxy = NULL;
	ssl_proxy_unref(proxy);
}

static void ssl_proxy_unref(struct ssl_proxy *proxy)
{
	if (--proxy->refcount > 0)
		return;
	i_assert(proxy->refcount == 0);

	SSL_free(proxy->ssl);

	pool_unref(&proxy->set_pool);
	i_free(proxy->last_error);
	i_free(proxy);
}

static void ssl_proxy_flush(struct ssl_proxy *proxy)
{
	/* this is pretty kludgy. mainly this is just for flushing the final
	   LOGOUT command output. */
	plain_read(proxy);
	ssl_step(proxy);
}

void ssl_proxy_destroy(struct ssl_proxy *proxy)
{
	if (proxy->destroyed || proxy->flushing)
		return;
	proxy->flushing = TRUE;
	ssl_proxy_flush(proxy);
	proxy->destroyed = TRUE;

	ssl_proxy_count--;
	DLLIST_REMOVE(&ssl_proxies, proxy);

	if (proxy->io_ssl_read != NULL)
		io_remove(&proxy->io_ssl_read);
	if (proxy->io_ssl_write != NULL)
		io_remove(&proxy->io_ssl_write);
	if (proxy->io_plain_read != NULL)
		io_remove(&proxy->io_plain_read);
	if (proxy->io_plain_write != NULL)
		io_remove(&proxy->io_plain_write);

	(void)SSL_shutdown(proxy->ssl);

	net_disconnect(proxy->fd_ssl);
	net_disconnect(proxy->fd_plain);

	if (proxy->client != NULL)
		client_unref(&proxy->client);
	ssl_proxy_unref(proxy);
}

static RSA *ssl_gen_rsa_key(SSL *ssl ATTR_UNUSED,
			    int is_export ATTR_UNUSED, int keylength)
{
	return RSA_generate_key(keylength, RSA_F4, NULL, NULL);
}

static DH *ssl_tmp_dh_callback(SSL *ssl ATTR_UNUSED,
			       int is_export, int keylength)
{
	if (is_export && keylength == 512 && ssl_params.dh_512 != NULL)
		return ssl_params.dh_512;

	return ssl_params.dh_default;
}

static void ssl_info_callback(const SSL *ssl, int where, int ret)
{
	struct ssl_proxy *proxy;

	proxy = SSL_get_ex_data(ssl, extdata_index);

	if (!proxy->ssl_set->verbose_ssl)
		return;

	if ((where & SSL_CB_ALERT) != 0) {
		switch (ret & 0xff) {
		case SSL_AD_CLOSE_NOTIFY:
			i_debug("SSL alert: %s [%s]",
				SSL_alert_desc_string_long(ret),
				net_ip2addr(&proxy->ip));
			break;
		default:
			i_warning("SSL alert: where=0x%x, ret=%d: %s %s [%s]",
				  where, ret, SSL_alert_type_string_long(ret),
				  SSL_alert_desc_string_long(ret),
				  net_ip2addr(&proxy->ip));
			break;
		}
	} else if (ret == 0) {
		i_warning("SSL failed: where=0x%x: %s [%s]",
			  where, SSL_state_string_long(ssl),
			  net_ip2addr(&proxy->ip));
	} else {
		i_debug("SSL: where=0x%x, ret=%d: %s [%s]",
			where, ret, SSL_state_string_long(ssl),
			net_ip2addr(&proxy->ip));
	}
}

static int ssl_verify_client_cert(int preverify_ok, X509_STORE_CTX *ctx)
{
	SSL *ssl;
        struct ssl_proxy *proxy;
	char buf[1024];
	X509_NAME *subject;

	ssl = X509_STORE_CTX_get_ex_data(ctx,
					 SSL_get_ex_data_X509_STORE_CTX_idx());
	proxy = SSL_get_ex_data(ssl, extdata_index);
	proxy->cert_received = TRUE;

	if (proxy->client_proxy && !proxy->login_set->ssl_require_crl &&
	    (ctx->error == X509_V_ERR_UNABLE_TO_GET_CRL ||
	     ctx->error == X509_V_ERR_CRL_HAS_EXPIRED)) {
		/* no CRL given with the CA list. don't worry about it. */
		preverify_ok = 1;
	}
	if (!preverify_ok)
		proxy->cert_broken = TRUE;

	subject = X509_get_subject_name(ctx->current_cert);
	(void)X509_NAME_oneline(subject, buf, sizeof(buf));
	buf[sizeof(buf)-1] = '\0'; /* just in case.. */

	if (proxy->cert_error == NULL) {
		proxy->cert_error = p_strdup_printf(proxy->client->pool, "%s: %s",
			X509_verify_cert_error_string(ctx->error), buf);
	}

	if (proxy->ssl_set->verbose_ssl ||
	    (proxy->login_set->auth_verbose && !preverify_ok)) {
		if (preverify_ok)
			i_info("Valid certificate: %s", buf);
		else {
			i_info("Invalid certificate: %s: %s",
			       X509_verify_cert_error_string(ctx->error), buf);
		}
	}

	/* Return success anyway, because if ssl_require_client_cert=no we
	   could still allow authentication. */
	return 1;
}

static int
pem_password_callback(char *buf, int size, int rwflag ATTR_UNUSED,
		      void *userdata)
{
	if (userdata == NULL) {
		i_error("SSL private key file is password protected, "
			"but password isn't given");
		return 0;
	}

	if (i_strocpy(buf, userdata, size) < 0)
		return 0;
	return strlen(buf);
}

unsigned int ssl_proxy_get_count(void)
{
	return ssl_proxy_count;
}

static bool is_pem_key(const char *cert)
{
	return strstr(cert, "PRIVATE KEY---") != NULL;
}

static void load_ca(X509_STORE *store, const char *ca,
		    STACK_OF(X509_NAME) **xnames_r)
{
	/* mostly just copy&pasted from X509_load_cert_crl_file() */
	STACK_OF(X509_INFO) *inf;
	X509_INFO *itmp;
	X509_NAME *xname;
	BIO *bio;
	int i;

	bio = BIO_new_mem_buf(t_strdup_noconst(ca), strlen(ca));
	if (bio == NULL)
		i_fatal("BIO_new_mem_buf() failed");
	inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
	if (inf == NULL)
		i_fatal("Couldn't parse ssl_ca: %s", ssl_last_error());
	BIO_free(bio);

	if (xnames_r != NULL) {
		*xnames_r = sk_X509_NAME_new_null();
		if (*xnames_r == NULL)
			i_fatal_status(FATAL_OUTOFMEM, "sk_X509_NAME_new_null() failed");
	}
	for(i = 0; i < sk_X509_INFO_num(inf); i++) {
		itmp = sk_X509_INFO_value(inf, i);
		if(itmp->x509) {
			X509_STORE_add_cert(store, itmp->x509);
			xname = X509_get_subject_name(itmp->x509);
			if (xname != NULL && xnames_r != NULL) {
				xname = X509_NAME_dup(xname);
				if (xname == NULL)
					i_fatal_status(FATAL_OUTOFMEM, "X509_NAME_dup() failed");
				sk_X509_NAME_push(*xnames_r, xname);
			}
		}
		if(itmp->crl)
			X509_STORE_add_crl(store, itmp->crl);
	}
	sk_X509_INFO_pop_free(inf, X509_INFO_free);
}

static STACK_OF(X509_NAME) *
ssl_proxy_ctx_init(SSL_CTX *ssl_ctx, const struct master_service_ssl_settings *set,
		   bool load_xnames)
{
	X509_STORE *store;
	STACK_OF(X509_NAME) *xnames = NULL;
	/* enable all SSL workarounds, except empty fragments as it
	   makes SSL more vulnerable against attacks */
	long ssl_ops = SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

#ifdef SSL_OP_NO_COMPRESSION
	if (!set->parsed_opts.compression)
		ssl_ops |= SSL_OP_NO_COMPRESSION;
#endif
	SSL_CTX_set_options(ssl_ctx, ssl_ops);

#ifdef SSL_MODE_RELEASE_BUFFERS
	SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
#endif

	if (*set->ssl_ca != '\0') {
		/* set trusted CA certs */
		store = SSL_CTX_get_cert_store(ssl_ctx);
		load_ca(store, set->ssl_ca, load_xnames ? &xnames : NULL);
	}
	ssl_proxy_ctx_set_crypto_params(ssl_ctx, set);
	SSL_CTX_set_info_callback(ssl_ctx, ssl_info_callback);
	return xnames;
}

static void
ssl_proxy_ctx_set_crypto_params(SSL_CTX *ssl_ctx,
	const struct master_service_ssl_settings *set ATTR_UNUSED)
{
#if defined(HAVE_ECDH) && !defined(SSL_CTRL_SET_ECDH_AUTO)
	EC_KEY *ecdh;
	int nid;
	const char *curve_name;
#endif
	if (SSL_CTX_need_tmp_RSA(ssl_ctx))
		SSL_CTX_set_tmp_rsa_callback(ssl_ctx, ssl_gen_rsa_key);
	SSL_CTX_set_tmp_dh_callback(ssl_ctx, ssl_tmp_dh_callback);
#ifdef HAVE_ECDH
	/* In the non-recommended situation where ECDH cipher suites are being
	   used instead of ECDHE, do not reuse the same ECDH key pair for
	   different sessions. This option improves forward secrecy. */
	SSL_CTX_set_options(ssl_ctx, SSL_OP_SINGLE_ECDH_USE);
#ifdef SSL_CTRL_SET_ECDH_AUTO
	/* OpenSSL >= 1.0.2 automatically handles ECDH temporary key parameter
	   selection. */
	SSL_CTX_set_ecdh_auto(ssl_ctx, 1);
#else
	/* For OpenSSL < 1.0.2, ECDH temporary key parameter selection must be
	   performed manually. Attempt to select the same curve as that used
	   in the server's private EC key file. Otherwise fall back to the
	   NIST P-384 (secp384r1) curve to be compliant with RFC 6460 when
	   AES-256 TLS cipher suites are in use. This fall back option does
	   however make Dovecot non-compliant with RFC 6460 which requires
	   curve NIST P-256 (prime256v1) be used when AES-128 TLS cipher
	   suites are in use. At least the non-compliance is in the form of
	   providing too much security rather than too little. */
	nid = ssl_proxy_ctx_get_pkey_ec_curve_name(set);
	ecdh = EC_KEY_new_by_curve_name(nid);
	if (ecdh == NULL) {
		/* Fall back option */
		nid = NID_secp384r1;
		ecdh = EC_KEY_new_by_curve_name(nid);
	}
	if ((curve_name = OBJ_nid2sn(nid)) != NULL && set->verbose_ssl)
		i_debug("SSL: elliptic curve %s will be used for ECDH and"
		        " ECDHE key exchanges", curve_name);
	if (ecdh != NULL) {
		SSL_CTX_set_tmp_ecdh(ssl_ctx, ecdh);
		EC_KEY_free(ecdh);
	}
#endif
#endif
}

static void
ssl_proxy_ctx_verify_client(SSL_CTX *ssl_ctx, STACK_OF(X509_NAME) *ca_names)
{
#if OPENSSL_VERSION_NUMBER >= 0x00907000L
	X509_STORE *store;

	store = SSL_CTX_get_cert_store(ssl_ctx);
	X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK |
			     X509_V_FLAG_CRL_CHECK_ALL);
#endif
	SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE,
			   ssl_verify_client_cert);
	/* set list of CA names that are sent to client */
	SSL_CTX_set_client_CA_list(ssl_ctx, ca_names);
}

static const char *ssl_proxy_get_use_certificate_error(const char *cert)
{
	unsigned long err;

	err = ERR_peek_error();
	if (ERR_GET_LIB(err) != ERR_LIB_PEM ||
	    ERR_GET_REASON(err) != PEM_R_NO_START_LINE)
		return ssl_last_error();
	else if (is_pem_key(cert)) {
		return "The file contains a private key "
			"(you've mixed ssl_cert and ssl_key settings)";
	} else if (strchr(cert, '\n') == NULL) {
		return t_strdup_printf("There is no valid PEM certificate. "
			"(You probably forgot '<' from ssl_cert=<%s)", cert);
	} else {
		return "There is no valid PEM certificate.";
	}
}

static const char *ssl_key_load_error(void)
{
	unsigned long err = ERR_peek_error();

	if (ERR_GET_LIB(err) == ERR_LIB_X509 &&
	    ERR_GET_REASON(err) == X509_R_KEY_VALUES_MISMATCH)
		return "Key is for a different cert than ssl_cert";
	else
		return ssl_last_error();
}

static EVP_PKEY * ATTR_NULL(2)
ssl_proxy_load_key(const char *key, const char *password)
{
	EVP_PKEY *pkey;
	BIO *bio;
	char *dup_password;

	bio = BIO_new_mem_buf(t_strdup_noconst(key), strlen(key));
	if (bio == NULL)
		i_fatal("BIO_new_mem_buf() failed");

	dup_password = t_strdup_noconst(password);
	pkey = PEM_read_bio_PrivateKey(bio, NULL, pem_password_callback,
				       dup_password);
	if (pkey == NULL) {
		i_fatal("Couldn't parse private ssl_key: %s",
			ssl_key_load_error());
	}
	BIO_free(bio);
	return pkey;
}

static void
ssl_proxy_ctx_use_key(SSL_CTX *ctx,
		      const struct master_service_ssl_settings *set)
{
	EVP_PKEY *pkey;
	const char *password;

	password = *set->ssl_key_password != '\0' ? set->ssl_key_password :
		getenv(MASTER_SSL_KEY_PASSWORD_ENV);
	pkey = ssl_proxy_load_key(set->ssl_key, password);
	if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1)
		i_fatal("Can't load private ssl_key: %s", ssl_key_load_error());
	EVP_PKEY_free(pkey);
}

#if defined(HAVE_ECDH) && !defined(SSL_CTRL_SET_ECDH_AUTO)
static int
ssl_proxy_ctx_get_pkey_ec_curve_name(const struct master_service_ssl_settings *set)
{
	int nid = 0;
	EVP_PKEY *pkey;
	const char *password;
	EC_KEY *eckey;
	const EC_GROUP *ecgrp;

	password = *set->ssl_key_password != '\0' ? set->ssl_key_password :
		getenv(MASTER_SSL_KEY_PASSWORD_ENV);
	pkey = ssl_proxy_load_key(set->ssl_key, password);
	if (pkey != NULL &&
	    (eckey = EVP_PKEY_get1_EC_KEY(pkey)) != NULL &&
	    (ecgrp = EC_KEY_get0_group(eckey)) != NULL)
		nid = EC_GROUP_get_curve_name(ecgrp);
	EVP_PKEY_free(pkey);
	return nid;
}
#endif

static int
ssl_proxy_ctx_use_certificate_chain(SSL_CTX *ctx, const char *cert)
{
	/* mostly just copy&pasted from SSL_CTX_use_certificate_chain_file() */
	BIO *in;
	X509 *x;
	int ret = 0;

	in = BIO_new_mem_buf(t_strdup_noconst(cert), strlen(cert));
	if (in == NULL)
		i_fatal("BIO_new_mem_buf() failed");

	x = PEM_read_bio_X509(in, NULL, NULL, NULL);
	if (x == NULL)
		goto end;

	ret = SSL_CTX_use_certificate(ctx, x);
#if 0
	/* This is in OpenSSL code, but it seems to cause failures.. */
	if (ERR_peek_error() != 0)
		ret = 0;
#endif

	if (ret != 0) {
		/* If we could set up our certificate, now proceed to
		 * the CA certificates.
		 */
		X509 *ca;
		int r;
		unsigned long err;
		
		while ((ca = PEM_read_bio_X509(in,NULL,NULL,NULL)) != NULL) {
			r = SSL_CTX_add_extra_chain_cert(ctx, ca);
			if (!r) {
				X509_free(ca);
				ret = 0;
				goto end;
			}
		}
		/* When the while loop ends, it's usually just EOF. */
		err = ERR_peek_last_error();
		if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE)
			ERR_clear_error();
		else 
			ret = 0; /* some real error */
		}

end:
	if (x != NULL) X509_free(x);
	BIO_free(in);
	return ret;
}

#ifdef HAVE_SSL_GET_SERVERNAME
static void ssl_servername_callback(SSL *ssl, int *al ATTR_UNUSED,
				    void *context ATTR_UNUSED)
{
	struct ssl_server_context *ctx;
	struct ssl_proxy *proxy;
	struct client *client;
	const char *host;
	void **other_sets;

	proxy = SSL_get_ex_data(ssl, extdata_index);
	host = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

	client = proxy->client;
	if (!client->ssl_servername_settings_read) {
		client->ssl_servername_settings_read = TRUE;
		client->set = login_settings_read(client->pool,
						  &client->local_ip,
						  &client->ip, host,
						  &client->ssl_set,
						  &other_sets);
	}
	ctx = ssl_server_context_get(client->set, client->ssl_set);
	SSL_set_SSL_CTX(ssl, ctx->ctx);
}
#endif

static struct ssl_server_context *
ssl_server_context_init(const struct login_settings *login_set,
			const struct master_service_ssl_settings *ssl_set)
{
	struct ssl_server_context *ctx;
	SSL_CTX *ssl_ctx;
	pool_t pool;
	STACK_OF(X509_NAME) *xnames;

	pool = pool_alloconly_create("ssl server context", 4096);
	ctx = p_new(pool, struct ssl_server_context, 1);
	ctx->pool = pool;
	ctx->cert = p_strdup(pool, ssl_set->ssl_cert);
	ctx->key = p_strdup(pool, ssl_set->ssl_key);
	ctx->ca = p_strdup(pool, ssl_set->ssl_ca);
	ctx->cipher_list = p_strdup(pool, ssl_set->ssl_cipher_list);
	ctx->protocols = p_strdup(pool, ssl_set->ssl_protocols);
	ctx->verify_client_cert = ssl_set->ssl_verify_client_cert ||
		login_set->auth_ssl_require_client_cert ||
		login_set->auth_ssl_username_from_cert;
	ctx->prefer_server_ciphers = ssl_set->ssl_prefer_server_ciphers;
	ctx->compression = ssl_set->parsed_opts.compression;

	ctx->ctx = ssl_ctx = SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx == NULL)
		i_fatal("SSL_CTX_new() failed");
	xnames = ssl_proxy_ctx_init(ssl_ctx, ssl_set, ctx->verify_client_cert);

	if (SSL_CTX_set_cipher_list(ssl_ctx, ctx->cipher_list) != 1) {
		i_fatal("Can't set cipher list to '%s': %s",
			ctx->cipher_list, ssl_last_error());
	}
	if (ctx->prefer_server_ciphers)
		SSL_CTX_set_options(ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
	SSL_CTX_set_options(ssl_ctx, openssl_get_protocol_options(ctx->protocols));

	if (ssl_proxy_ctx_use_certificate_chain(ctx->ctx, ctx->cert) != 1) {
		i_fatal("Can't load ssl_cert: %s",
			ssl_proxy_get_use_certificate_error(ctx->cert));
	}

#ifdef HAVE_SSL_GET_SERVERNAME
	if (SSL_CTX_set_tlsext_servername_callback(ctx->ctx,
						   ssl_servername_callback) != 1) {
		if (ssl_set->verbose_ssl)
			i_debug("OpenSSL library doesn't support SNI");
	}
#endif

	ssl_proxy_ctx_use_key(ctx->ctx, ssl_set);

	if (ctx->verify_client_cert)
		ssl_proxy_ctx_verify_client(ctx->ctx, xnames);

	i_assert(hash_table_lookup(ssl_servers, ctx) == NULL);
	hash_table_insert(ssl_servers, ctx, ctx);
	return ctx;
}

static void ssl_server_context_deinit(struct ssl_server_context **_ctx)
{
	struct ssl_server_context *ctx = *_ctx;

	SSL_CTX_free(ctx->ctx);
	pool_unref(&ctx->pool);
}

static void
ssl_proxy_client_ctx_set_client_cert(SSL_CTX *ctx,
				     const struct login_settings *set)
{
	EVP_PKEY *pkey;

	if (*set->ssl_client_cert == '\0')
		return;

	if (ssl_proxy_ctx_use_certificate_chain(ctx, set->ssl_client_cert) != 1) {
		i_fatal("Can't load ssl_client_cert: %s",
			ssl_proxy_get_use_certificate_error(set->ssl_client_cert));
	}

	pkey = ssl_proxy_load_key(set->ssl_client_key, NULL);
	if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
		i_fatal("Can't load private ssl_client_key: %s",
			ssl_key_load_error());
	}
	EVP_PKEY_free(pkey);
}

static void
ssl_proxy_init_client(const struct login_settings *login_set,
		      const struct master_service_ssl_settings *ssl_set)
{
	STACK_OF(X509_NAME) *xnames;

	if ((ssl_client_ctx = SSL_CTX_new(SSLv23_client_method())) == NULL)
		i_fatal("SSL_CTX_new() failed");
	xnames = ssl_proxy_ctx_init(ssl_client_ctx, ssl_set, TRUE);
	ssl_proxy_ctx_verify_client(ssl_client_ctx, xnames);

	ssl_proxy_client_ctx_set_client_cert(ssl_client_ctx, login_set);
}

void ssl_proxy_init(void)
{
	const struct login_settings *login_set = global_login_settings;
	const struct master_service_ssl_settings *ssl_set = global_ssl_settings;
	static char dovecot[] = "dovecot";
	unsigned char buf;

	if (strcmp(ssl_set->ssl, "no") == 0)
		return;

	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();

	if (*ssl_set->ssl_crypto_device != '\0') {
		ENGINE_load_builtin_engines();
		ssl_engine = ENGINE_by_id(ssl_set->ssl_crypto_device);
		if (ssl_engine == NULL) {
			i_fatal("Unknown ssl_crypto_device: %s",
				ssl_set->ssl_crypto_device);
		}
		ENGINE_init(ssl_engine);
		ENGINE_set_default_RSA(ssl_engine);
		ENGINE_set_default_DSA(ssl_engine);
		ENGINE_set_default_ciphers(ssl_engine);
	}

	extdata_index = SSL_get_ex_new_index(0, dovecot, NULL, NULL, NULL);

	hash_table_create(&ssl_servers, default_pool, 0,
			  ssl_server_context_hash, ssl_server_context_cmp);
	(void)ssl_server_context_init(login_set, ssl_set);

	ssl_proxy_init_client(login_set, ssl_set);
	ssl_username_nid = OBJ_txt2nid(ssl_set->ssl_cert_username_field);
	if (ssl_username_nid == NID_undef) {
		i_fatal("Invalid ssl_cert_username_field: %s",
			ssl_set->ssl_cert_username_field);
	}

	/* PRNG initialization might want to use /dev/urandom, make sure it
	   does it before chrooting. We might not have enough entropy at
	   the first try, so this function may fail. It's still been
	   initialized though. */
	(void)RAND_bytes(&buf, 1);

	memset(&ssl_params, 0, sizeof(ssl_params));
	ssl_params.path = SSL_PARAMETERS_PATH;

	ssl_proxy_count = 0;
        ssl_proxies = NULL;
	ssl_initialized = TRUE;
}

void ssl_proxy_deinit(void)
{
	struct hash_iterate_context *iter;
	struct ssl_server_context *ctx;

	if (!ssl_initialized)
		return;

	while (ssl_proxies != NULL)
		ssl_proxy_destroy(ssl_proxies);

	iter = hash_table_iterate_init(ssl_servers);
	while (hash_table_iterate(iter, ssl_servers, &ctx, &ctx))
		ssl_server_context_deinit(&ctx);
	hash_table_iterate_deinit(&iter);
	hash_table_destroy(&ssl_servers);

	ssl_free_parameters(&ssl_params);
	SSL_CTX_free(ssl_client_ctx);
	if (ssl_engine != NULL) {
		ENGINE_finish(ssl_engine);
		ENGINE_cleanup();
	}
	EVP_cleanup();
	ERR_free_strings();
}

#endif
