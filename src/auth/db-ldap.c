/* Copyright (c) 2003-2014 Dovecot authors, see the included COPYING file */

#include "auth-common.h"

#if defined(BUILTIN_LDAP) || defined(PLUGIN_BUILD)

#include "net.h"
#include "ioloop.h"
#include "array.h"
#include "hash.h"
#include "aqueue.h"
#include "str.h"
#include "time-util.h"
#include "env-util.h"
#include "var-expand.h"
#include "settings.h"
#include "userdb.h"
#include "db-ldap.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#define HAVE_LDAP_SASL
#ifdef HAVE_SASL_SASL_H
#  include <sasl/sasl.h>
#elif defined (HAVE_SASL_H)
#  include <sasl.h>
#else
#  undef HAVE_LDAP_SASL
#endif
#ifdef LDAP_OPT_X_TLS
#  define OPENLDAP_TLS_OPTIONS
#endif
#if SASL_VERSION_MAJOR < 2
#  undef HAVE_LDAP_SASL
#endif

#ifndef LDAP_SASL_QUIET
#  define LDAP_SASL_QUIET 0 /* Doesn't exist in Solaris LDAP */
#endif

/* Older versions may require calling ldap_result() twice */
#if LDAP_VENDOR_VERSION <= 20112
#  define OPENLDAP_ASYNC_WORKAROUND
#endif

/* Solaris LDAP library doesn't have LDAP_OPT_SUCCESS */
#ifndef LDAP_OPT_SUCCESS
#  define LDAP_OPT_SUCCESS LDAP_SUCCESS
#endif

struct db_ldap_result {
	int refcount;
	LDAPMessage *msg;
};

struct db_ldap_value {
	const char **values;
	bool used;
};

struct db_ldap_result_iterate_context {
	pool_t pool;

	struct auth_request *auth_request;
	const ARRAY_TYPE(ldap_field) *attr_map;
	unsigned int attr_idx;

	/* attribute name => value */
	HASH_TABLE(char *, struct db_ldap_value *) ldap_attrs;

	const char *val_1_arr[2];
	string_t *var, *debug;

	bool skip_null_values;
	bool iter_dn_values;
};

struct db_ldap_sasl_bind_context {
	const char *authcid;
	const char *passwd;
	const char *realm;
	const char *authzid;
};

#define DEF_STR(name) DEF_STRUCT_STR(name, ldap_settings)
#define DEF_INT(name) DEF_STRUCT_INT(name, ldap_settings)
#define DEF_BOOL(name) DEF_STRUCT_BOOL(name, ldap_settings)

static struct setting_def setting_defs[] = {
	DEF_STR(hosts),
	DEF_STR(uris),
	DEF_STR(dn),
	DEF_STR(dnpass),
	DEF_BOOL(auth_bind),
	DEF_STR(auth_bind_userdn),
	DEF_BOOL(tls),
	DEF_BOOL(sasl_bind),
	DEF_STR(sasl_mech),
	DEF_STR(sasl_realm),
	DEF_STR(sasl_authz_id),
	DEF_STR(tls_ca_cert_file),
	DEF_STR(tls_ca_cert_dir),
	DEF_STR(tls_cert_file),
	DEF_STR(tls_key_file),
	DEF_STR(tls_cipher_suite),
	DEF_STR(tls_require_cert),
	DEF_STR(deref),
	DEF_STR(scope),
	DEF_STR(base),
	DEF_INT(ldap_version),
	DEF_STR(debug_level),
	DEF_STR(ldaprc_path),
	DEF_STR(user_attrs),
	DEF_STR(user_filter),
	DEF_STR(pass_attrs),
	DEF_STR(pass_filter),
	DEF_STR(iterate_attrs),
	DEF_STR(iterate_filter),
	DEF_STR(default_pass_scheme),
	DEF_BOOL(userdb_warning_disable),
	DEF_BOOL(blocking),

	{ 0, NULL, 0 }
};

static struct ldap_settings default_ldap_settings = {
	.hosts = NULL,
	.uris = NULL,
	.dn = NULL,
	.dnpass = NULL,
	.auth_bind = FALSE,
	.auth_bind_userdn = NULL,
	.tls = FALSE,
	.sasl_bind = FALSE,
	.sasl_mech = NULL,
	.sasl_realm = NULL,
	.sasl_authz_id = NULL,
	.tls_ca_cert_file = NULL,
	.tls_ca_cert_dir = NULL,
	.tls_cert_file = NULL,
	.tls_key_file = NULL,
	.tls_cipher_suite = NULL,
	.tls_require_cert = NULL,
	.deref = "never",
	.scope = "subtree",
	.base = NULL,
	.ldap_version = 3,
	.debug_level = "0",
	.ldaprc_path = "",
	.user_attrs = "homeDirectory=home,uidNumber=uid,gidNumber=gid",
	.user_filter = "(&(objectClass=posixAccount)(uid=%u))",
	.pass_attrs = "uid=user,userPassword=password",
	.pass_filter = "(&(objectClass=posixAccount)(uid=%u))",
	.iterate_attrs = "uid=user",
	.iterate_filter = "(objectClass=posixAccount)",
	.default_pass_scheme = "crypt",
	.userdb_warning_disable = FALSE,
	.blocking = FALSE
};

static struct ldap_connection *ldap_connections = NULL;

static int db_ldap_bind(struct ldap_connection *conn);
static void db_ldap_conn_close(struct ldap_connection *conn);
struct db_ldap_result_iterate_context *
db_ldap_result_iterate_init_full(struct ldap_connection *conn,
				 struct ldap_request_search *ldap_request,
				 LDAPMessage *res, bool skip_null_values,
				 bool iter_dn_values);

static int deref2str(const char *str)
{
	if (strcasecmp(str, "never") == 0)
		return LDAP_DEREF_NEVER;
	if (strcasecmp(str, "searching") == 0)
		return LDAP_DEREF_SEARCHING;
	if (strcasecmp(str, "finding") == 0)
		return LDAP_DEREF_FINDING;
	if (strcasecmp(str, "always") == 0)
		return LDAP_DEREF_ALWAYS;

	i_fatal("LDAP: Unknown deref option '%s'", str);
}

static int scope2str(const char *str)
{
	if (strcasecmp(str, "base") == 0)
		return LDAP_SCOPE_BASE;
	if (strcasecmp(str, "onelevel") == 0)
		return LDAP_SCOPE_ONELEVEL;
	if (strcasecmp(str, "subtree") == 0)
		return LDAP_SCOPE_SUBTREE;

	i_fatal("LDAP: Unknown scope option '%s'", str);
}

#ifdef OPENLDAP_TLS_OPTIONS
static int tls_require_cert2str(const char *str)
{
	if (strcasecmp(str, "never") == 0)
		return LDAP_OPT_X_TLS_NEVER;
	if (strcasecmp(str, "hard") == 0)
		return LDAP_OPT_X_TLS_HARD;
	if (strcasecmp(str, "demand") == 0)
		return LDAP_OPT_X_TLS_DEMAND;
	if (strcasecmp(str, "allow") == 0)
		return LDAP_OPT_X_TLS_ALLOW;
	if (strcasecmp(str, "try") == 0)
		return LDAP_OPT_X_TLS_TRY;

	i_fatal("LDAP: Unknown tls_require_cert value '%s'", str);
}
#endif

static int ldap_get_errno(struct ldap_connection *conn)
{
	int ret, err;

	ret = ldap_get_option(conn->ld, LDAP_OPT_ERROR_NUMBER, (void *) &err);
	if (ret != LDAP_SUCCESS) {
		i_error("LDAP: Can't get error number: %s",
			ldap_err2string(ret));
		return LDAP_UNAVAILABLE;
	}

	return err;
}

const char *ldap_get_error(struct ldap_connection *conn)
{
	const char *ret;
	char *str = NULL;

	ret = ldap_err2string(ldap_get_errno(conn));

	ldap_get_option(conn->ld, LDAP_OPT_ERROR_STRING, (void *)&str);
	if (str != NULL) {
		ret = t_strconcat(ret, ", ", str, NULL);
		ldap_memfree(str);
	}
	ldap_set_option(conn->ld, LDAP_OPT_ERROR_STRING, NULL);
	return ret;
}

static void ldap_conn_reconnect(struct ldap_connection *conn)
{
	db_ldap_conn_close(conn);
	if (db_ldap_connect(conn) < 0)
		db_ldap_conn_close(conn);
}

static int ldap_handle_error(struct ldap_connection *conn)
{
	int err = ldap_get_errno(conn);

	switch (err) {
	case LDAP_SUCCESS:
		i_unreached();
	case LDAP_SIZELIMIT_EXCEEDED:
	case LDAP_TIMELIMIT_EXCEEDED:
	case LDAP_NO_SUCH_ATTRIBUTE:
	case LDAP_UNDEFINED_TYPE:
	case LDAP_INAPPROPRIATE_MATCHING:
	case LDAP_CONSTRAINT_VIOLATION:
	case LDAP_TYPE_OR_VALUE_EXISTS:
	case LDAP_INVALID_SYNTAX:
	case LDAP_NO_SUCH_OBJECT:
	case LDAP_ALIAS_PROBLEM:
	case LDAP_INVALID_DN_SYNTAX:
	case LDAP_IS_LEAF:
	case LDAP_ALIAS_DEREF_PROBLEM:
	case LDAP_FILTER_ERROR:
		/* invalid input */
		return -1;
	case LDAP_SERVER_DOWN:
	case LDAP_TIMEOUT:
	case LDAP_UNAVAILABLE:
	case LDAP_BUSY:
#ifdef LDAP_CONNECT_ERROR
	case LDAP_CONNECT_ERROR:
#endif
	case LDAP_LOCAL_ERROR:
	case LDAP_INVALID_CREDENTIALS:
	case LDAP_OPERATIONS_ERROR:
	default:
		/* connection problems */
		ldap_conn_reconnect(conn);
		return 0;
	}
}

static int db_ldap_request_bind(struct ldap_connection *conn,
				struct ldap_request *request)
{
	struct ldap_request_bind *brequest =
		(struct ldap_request_bind *)request;

	i_assert(request->type == LDAP_REQUEST_TYPE_BIND);
	i_assert(request->msgid == -1);
	i_assert(conn->conn_state == LDAP_CONN_STATE_BOUND_AUTH ||
		 conn->conn_state == LDAP_CONN_STATE_BOUND_DEFAULT);
	i_assert(conn->pending_count == 0);

	request->msgid = ldap_bind(conn->ld, brequest->dn,
				   request->auth_request->mech_password,
				   LDAP_AUTH_SIMPLE);
	if (request->msgid == -1) {
		auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
				       "ldap_bind(%s) failed: %s",
				       brequest->dn, ldap_get_error(conn));
		if (ldap_handle_error(conn) < 0) {
			/* broken request, remove it */
			return 0;
		}
		return -1;
	}
	conn->conn_state = LDAP_CONN_STATE_BINDING;
	return 1;
}

static int db_ldap_request_search(struct ldap_connection *conn,
				  struct ldap_request *request)
{
	struct ldap_request_search *srequest =
		(struct ldap_request_search *)request;

	i_assert(conn->conn_state == LDAP_CONN_STATE_BOUND_DEFAULT);
	i_assert(request->msgid == -1);

	request->msgid =
		ldap_search(conn->ld, *srequest->base == '\0' ? NULL :
			    srequest->base, conn->set.ldap_scope,
			    srequest->filter, srequest->attributes, 0);
	if (request->msgid == -1) {
		auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
				       "ldap_search(%s) parsing failed: %s",
				       srequest->filter, ldap_get_error(conn));
		if (ldap_handle_error(conn) < 0) {
			/* broken request, remove it */
			return 0;
		}
		return -1;
	}
	return 1;
}

static bool db_ldap_request_queue_next(struct ldap_connection *conn)
{
	struct ldap_request *const *requestp, *request;
	int ret = -1;

	/* connecting may call db_ldap_connect_finish(), which gets us back
	   here. so do the connection before checking the request queue. */
	if (db_ldap_connect(conn) < 0)
		return FALSE;

	if (conn->pending_count == aqueue_count(conn->request_queue)) {
		/* no non-pending requests */
		return FALSE;
	}
	if (conn->pending_count > DB_LDAP_MAX_PENDING_REQUESTS) {
		/* wait until server has replied to some requests */
		return FALSE;
	}

	requestp = array_idx(&conn->request_array,
			     aqueue_idx(conn->request_queue,
					conn->pending_count));
	request = *requestp;

	if (conn->pending_count > 0 &&
	    request->type == LDAP_REQUEST_TYPE_BIND) {
		/* we can't do binds until all existing requests are finished */
		return FALSE;
	}

	switch (conn->conn_state) {
	case LDAP_CONN_STATE_DISCONNECTED:
	case LDAP_CONN_STATE_BINDING:
		/* wait until we're in bound state */
		return FALSE;
	case LDAP_CONN_STATE_BOUND_AUTH:
		if (request->type == LDAP_REQUEST_TYPE_BIND)
			break;

		/* bind to default dn first */
		i_assert(conn->pending_count == 0);
		(void)db_ldap_bind(conn);
		return FALSE;
	case LDAP_CONN_STATE_BOUND_DEFAULT:
		/* we can do anything in this state */
		break;
	}

	switch (request->type) {
	case LDAP_REQUEST_TYPE_BIND:
		ret = db_ldap_request_bind(conn, request);
		break;
	case LDAP_REQUEST_TYPE_SEARCH:
		ret = db_ldap_request_search(conn, request);
		break;
	}

	if (ret > 0) {
		/* success */
		i_assert(request->msgid != -1);
		conn->pending_count++;
		return TRUE;
	} else if (ret < 0) {
		/* disconnected */
		return FALSE;
	} else {
		/* broken request, remove from queue */
		aqueue_delete_tail(conn->request_queue);
		request->callback(conn, request, NULL);
		return TRUE;
	}
}

static bool
db_ldap_check_limits(struct ldap_connection *conn, struct ldap_request *request)
{
	struct ldap_request *const *first_requestp;
	unsigned int count;
	time_t secs_diff;

	count = aqueue_count(conn->request_queue);
	if (count == 0)
		return TRUE;

	first_requestp = array_idx(&conn->request_array,
				   aqueue_idx(conn->request_queue, 0));
	secs_diff = ioloop_time - (*first_requestp)->create_time;
	if (secs_diff > DB_LDAP_REQUEST_LOST_TIMEOUT_SECS) {
		auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
			"Connection appears to be hanging, reconnecting");
		ldap_conn_reconnect(conn);
		return TRUE;
	}
	return TRUE;
}

void db_ldap_request(struct ldap_connection *conn,
		     struct ldap_request *request)
{
	i_assert(request->auth_request != NULL);

	request->msgid = -1;
	request->create_time = ioloop_time;

	if (!db_ldap_check_limits(conn, request)) {
		request->callback(conn, request, NULL);
		return;
	}

	aqueue_append(conn->request_queue, &request);
	(void)db_ldap_request_queue_next(conn);
}

static int db_ldap_connect_finish(struct ldap_connection *conn, int ret)
{
	if (ret == LDAP_SERVER_DOWN) {
		i_error("LDAP: Can't connect to server: %s",
			conn->set.uris != NULL ?
			conn->set.uris : conn->set.hosts);
		return -1;
	}
	if (ret != LDAP_SUCCESS) {
		i_error("LDAP: binding failed (dn %s): %s",
			conn->set.dn == NULL ? "(none)" : conn->set.dn,
			ldap_get_error(conn));
		return -1;
	}

	if (conn->to != NULL)
		timeout_remove(&conn->to);
	conn->conn_state = LDAP_CONN_STATE_BOUND_DEFAULT;
	while (db_ldap_request_queue_next(conn))
		;
	return 0;
}

static void db_ldap_default_bind_finished(struct ldap_connection *conn,
					  struct db_ldap_result *res)
{
	int ret;

	i_assert(conn->pending_count == 0);
	conn->default_bind_msgid = -1;

	ret = ldap_result2error(conn->ld, res->msg, FALSE);
	if (db_ldap_connect_finish(conn, ret) < 0) {
		/* lost connection, close it */
		db_ldap_conn_close(conn);
	}
}

static void db_ldap_abort_requests(struct ldap_connection *conn,
				   unsigned int max_count,
				   unsigned int timeout_secs,
				   bool error, const char *reason)
{
	struct ldap_request *const *requestp, *request;
	time_t diff;

	while (aqueue_count(conn->request_queue) > 0 && max_count > 0) {
		requestp = array_idx(&conn->request_array,
				     aqueue_idx(conn->request_queue, 0));
		request = *requestp;

		diff = ioloop_time - request->create_time;
		if (diff < (time_t)timeout_secs)
			break;

		/* timed out, abort */
		aqueue_delete_tail(conn->request_queue);

		if (request->msgid != -1) {
			i_assert(conn->pending_count > 0);
			conn->pending_count--;
		}
		if (error) {
			auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
					       "%s", reason);
		} else {
			auth_request_log_info(request->auth_request, AUTH_SUBSYS_DB,
					      "%s", reason);
		}
		request->callback(conn, request, NULL);
		max_count--;
	}
}

static struct ldap_request *
db_ldap_find_request(struct ldap_connection *conn, int msgid,
		     unsigned int *idx_r)
{
	struct ldap_request *const *requests, *request = NULL;
	unsigned int i, count;

	count = aqueue_count(conn->request_queue);
	if (count == 0)
		return NULL;

	requests = array_idx(&conn->request_array, 0);
	for (i = 0; i < count; i++) {
		request = requests[aqueue_idx(conn->request_queue, i)];
		if (request->msgid == msgid) {
			*idx_r = i;
			return request;
		}
		if (request->msgid == -1)
			break;
	}
	return NULL;
}

static int db_ldap_fields_get_dn(struct ldap_connection *conn,
				 struct ldap_request_search *request,
				 struct db_ldap_result *res)
{
	struct auth_request *auth_request = request->request.auth_request;
	struct ldap_request_named_result *named_res;
	struct db_ldap_result_iterate_context *ldap_iter;
	const char *name, *const *values;

	ldap_iter = db_ldap_result_iterate_init_full(conn, request, res->msg,
						     TRUE, TRUE);
	while (db_ldap_result_iterate_next(ldap_iter, &name, &values)) {
		if (values[1] != NULL) {
			auth_request_log_warning(auth_request, AUTH_SUBSYS_DB,
				"Multiple values found for '%s', "
				"using value '%s'", name, values[0]);
		}
		array_foreach_modifiable(&request->named_results, named_res) {
			if (strcmp(named_res->field->name, name) != 0)
				continue;
			/* In future we could also support LDAP URLs here */
			named_res->dn = p_strdup(auth_request->pool,
						 values[0]);
		}
	}
	db_ldap_result_iterate_deinit(&ldap_iter);
	return 0;
}

struct ldap_field_find_subquery_context {
	ARRAY_TYPE(string) attr_names;
	const char *name;
};

static const char *
db_ldap_field_subquery_find(const char *data, void *context)
{
	struct ldap_field_find_subquery_context *ctx = context;
	char *ldap_attr;
	const char *p;

	if (*data != '\0') {
		data = t_strcut(data, ':');
		p = strchr(data, '@');
		if (p != NULL && strcmp(p+1, ctx->name) == 0) {
			ldap_attr = p_strdup_until(unsafe_data_stack_pool,
						   data, p);
			array_append(&ctx->attr_names, &ldap_attr, 1);
		}
	}
	return NULL;
}


static int
ldap_request_send_subquery(struct ldap_connection *conn,
			   struct ldap_request_search *request,
			   struct ldap_request_named_result *named_res)
{
	static struct var_expand_func_table var_funcs_table[] = {
		{ "ldap", db_ldap_field_subquery_find },
		{ "ldap_ptr", db_ldap_field_subquery_find },
		{ NULL, NULL }
	};
	const struct ldap_field *field;
	const char *p;
	char *name;
	struct ldap_field_find_subquery_context ctx;
	string_t *tmp_str = t_str_new(64);

	memset(&ctx, 0, sizeof(ctx));
	t_array_init(&ctx.attr_names, 8);
	ctx.name = named_res->field->name;

	/* get the attributes names into array (ldapAttr@name -> ldapAttr) */
	array_foreach(request->attr_map, field) {
		if (field->ldap_attr_name[0] == '\0') {
			str_truncate(tmp_str, 0);
			var_expand_with_funcs(tmp_str, field->value, NULL,
					      var_funcs_table, &ctx);
		} else {
			p = strchr(field->ldap_attr_name, '@');
			if (p != NULL &&
			    strcmp(p+1, named_res->field->name) == 0) {
				name = p_strdup_until(unsafe_data_stack_pool,
						      field->ldap_attr_name, p);
				array_append(&ctx.attr_names, &name, 1);
			}
		}
	}
	array_append_zero(&ctx.attr_names);

	request->request.msgid =
		ldap_search(conn->ld, named_res->dn, LDAP_SCOPE_BASE,
			    NULL, array_idx_modifiable(&ctx.attr_names, 0), 0);
	if (request->request.msgid == -1) {
		auth_request_log_error(request->request.auth_request, AUTH_SUBSYS_DB,
				       "ldap_search(dn=%s) failed: %s",
				       named_res->dn, ldap_get_error(conn));
		return -1;
	}
	return 0;
}

static int db_ldap_search_save_result(struct ldap_request_search *request,
				      struct db_ldap_result *res)
{
	struct ldap_request_named_result *named_res;

	if (!array_is_created(&request->named_results)) {
		if (request->result != NULL)
			return -1;
		request->result = res;
	} else {
		named_res = array_idx_modifiable(&request->named_results,
						 request->name_idx);
		if (named_res->result != NULL)
			return -1;
		named_res->result = res;
	}
	res->refcount++;
	return 0;
}

static int db_ldap_search_next_subsearch(struct ldap_connection *conn,
					 struct ldap_request_search *request,
					 struct db_ldap_result *res)
{
	struct ldap_request_named_result *named_res;
	const struct ldap_field *field;

	if (request->result != NULL)
		res = request->result;

	if (!array_is_created(&request->named_results)) {
		/* see if we need to do more LDAP queries */
		p_array_init(&request->named_results,
			     request->request.auth_request->pool, 2);
		array_foreach(request->attr_map, field) {
			if (!field->value_is_dn)
				continue;
			named_res = array_append_space(&request->named_results);
			named_res->field = field;
		}
		if (db_ldap_fields_get_dn(conn, request, res) < 0)
			return -1;
	} else {
		request->name_idx++;
	}
	while (request->name_idx < array_count(&request->named_results)) {
		/* send the next LDAP query */
		named_res = array_idx_modifiable(&request->named_results,
						 request->name_idx);
		if (named_res->dn != NULL) {
			if (ldap_request_send_subquery(conn, request,
						       named_res) < 0)
				return -1;
			return 1;
		}
		/* dn field wasn't returned, skip this */
		request->name_idx++;
	}
	return 0;
}

static bool
db_ldap_handle_request_result(struct ldap_connection *conn,
			      struct ldap_request *request, unsigned int idx,
			      struct db_ldap_result *res)
{
	struct ldap_request_search *srequest = NULL;
	const struct ldap_request_named_result *named_res;
	int ret;
	bool final_result;

	i_assert(conn->pending_count > 0);

	if (request->type == LDAP_REQUEST_TYPE_BIND) {
		i_assert(conn->conn_state == LDAP_CONN_STATE_BINDING);
		i_assert(conn->pending_count == 1);
		conn->conn_state = LDAP_CONN_STATE_BOUND_AUTH;
	} else {
		srequest = (struct ldap_request_search *)request;
		switch (ldap_msgtype(res->msg)) {
		case LDAP_RES_SEARCH_ENTRY:
		case LDAP_RES_SEARCH_RESULT:
			break;
		case LDAP_RES_SEARCH_REFERENCE:
			/* we're going to ignore this */
			return FALSE;
		default:
			i_error("LDAP: Reply with unexpected type %d",
				ldap_msgtype(res->msg));
			return TRUE;
		}
	}
	if (ldap_msgtype(res->msg) == LDAP_RES_SEARCH_ENTRY) {
		ret = LDAP_SUCCESS;
		final_result = FALSE;
	} else {
		final_result = TRUE;
		ret = ldap_result2error(conn->ld, res->msg, 0);
	}
	/* LDAP_NO_SUCH_OBJECT is returned for nonexistent base */
	if (ret != LDAP_SUCCESS && ret != LDAP_NO_SUCH_OBJECT &&
	    request->type == LDAP_REQUEST_TYPE_SEARCH) {
		/* handle search failures here */
		struct ldap_request_search *srequest =
			(struct ldap_request_search *)request;

		if (!array_is_created(&srequest->named_results)) {
			auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
				"ldap_search(base=%s filter=%s) failed: %s",
				srequest->base, srequest->filter,
				ldap_err2string(ret));
		} else {
			named_res = array_idx(&srequest->named_results,
					      srequest->name_idx);
			auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
				"ldap_search(base=%s) failed: %s",
				named_res->dn, ldap_err2string(ret));
		}
		res = NULL;
	}
	if (ret == LDAP_SUCCESS && srequest != NULL && !srequest->multi_entry) {
		/* expand any @results */
		if (!final_result) {
			if (db_ldap_search_save_result(srequest, res) < 0) {
				auth_request_log_error(request->auth_request, AUTH_SUBSYS_DB,
					"LDAP search returned multiple entries");
				res = NULL;
			} else {
				/* wait for finish */
				return FALSE;
			}
		} else {
			ret = db_ldap_search_next_subsearch(conn, srequest, res);
			if (ret > 0) {
				/* more LDAP queries left */
				return FALSE;
			}
			if (ret < 0)
				res = NULL;
		}
	}
	if (res == NULL && !final_result) {
		/* wait for the final reply */
		request->failed = TRUE;
		return TRUE;
	}
	if (request->failed)
		res = NULL;
	if (final_result) {
		conn->pending_count--;
		aqueue_delete(conn->request_queue, idx);
	}

	T_BEGIN {
		if (res != NULL && srequest != NULL && srequest->result != NULL)
			request->callback(conn, request, srequest->result->msg);

		request->callback(conn, request, res == NULL ? NULL : res->msg);
	} T_END;

	if (idx > 0) {
		/* see if there are timed out requests */
		db_ldap_abort_requests(conn, idx,
				       DB_LDAP_REQUEST_LOST_TIMEOUT_SECS,
				       TRUE, "Request lost");
	}
	return TRUE;
}

static void db_ldap_result_unref(struct db_ldap_result **_res)
{
	struct db_ldap_result *res = *_res;

	*_res = NULL;
	i_assert(res->refcount > 0);
	if (--res->refcount == 0) {
		ldap_msgfree(res->msg);
		i_free(res);
	}
}

static void
db_ldap_request_free(struct ldap_request *request)
{
	if (request->type == LDAP_REQUEST_TYPE_SEARCH) {
		struct ldap_request_search *srequest =
			(struct ldap_request_search *)request;
		struct ldap_request_named_result *named_res;

		if (srequest->result != NULL)
			db_ldap_result_unref(&srequest->result);

		if (array_is_created(&srequest->named_results)) {
			array_foreach_modifiable(&srequest->named_results, named_res) {
				if (named_res->result != NULL)
					db_ldap_result_unref(&named_res->result);
			}
			array_clear(&srequest->named_results);
		}
	}
}

static void
db_ldap_handle_result(struct ldap_connection *conn, struct db_ldap_result *res)
{
	struct auth_request *auth_request;
	struct ldap_request *request;
	unsigned int idx;
	int msgid;

	msgid = ldap_msgid(res->msg);
	if (msgid == conn->default_bind_msgid) {
		db_ldap_default_bind_finished(conn, res);
		return;
	}

	request = db_ldap_find_request(conn, msgid, &idx);
	if (request == NULL) {
		i_error("LDAP: Reply with unknown msgid %d", msgid);
		return;
	}
	/* request is allocated from auth_request's pool */
	auth_request = request->auth_request;
	auth_request_ref(auth_request);
	if (db_ldap_handle_request_result(conn, request, idx, res))
		db_ldap_request_free(request);
	auth_request_unref(&auth_request);
}

static void ldap_input(struct ldap_connection *conn)
{
	struct timeval timeout;
	struct db_ldap_result *res;
	LDAPMessage *msg;
	time_t prev_reply_diff;
	int ret;

	do {
		if (conn->ld == NULL)
			return;

		memset(&timeout, 0, sizeof(timeout));
		ret = ldap_result(conn->ld, LDAP_RES_ANY, 0, &timeout, &msg);
#ifdef OPENLDAP_ASYNC_WORKAROUND
		if (ret == 0) {
			/* try again, there may be another in buffer */
			ret = ldap_result(conn->ld, LDAP_RES_ANY, 0,
					  &timeout, &msg);
		}
#endif
		if (ret <= 0)
			break;

		res = i_new(struct db_ldap_result, 1);
		res->refcount = 1;
		res->msg = msg;
		db_ldap_handle_result(conn, res);
		db_ldap_result_unref(&res);
	} while (conn->io != NULL);

	prev_reply_diff = ioloop_time - conn->last_reply_stamp;
	conn->last_reply_stamp = ioloop_time;

	if (ret > 0) {
		/* input disabled, continue once it's enabled */
		i_assert(conn->io == NULL);
	} else if (ret == 0) {
		/* send more requests */
		while (db_ldap_request_queue_next(conn))
			;
	} else if (ldap_get_errno(conn) != LDAP_SERVER_DOWN) {
		i_error("LDAP: ldap_result() failed: %s", ldap_get_error(conn));
		ldap_conn_reconnect(conn);
	} else if (aqueue_count(conn->request_queue) > 0 ||
		   prev_reply_diff < DB_LDAP_IDLE_RECONNECT_SECS) {
		i_error("LDAP: Connection lost to LDAP server, reconnecting");
		ldap_conn_reconnect(conn);
	} else {
		/* server probably disconnected an idle connection. don't
		   reconnect until the next request comes. */
		db_ldap_conn_close(conn);
	}
}

#ifdef HAVE_LDAP_SASL
static int
sasl_interact(LDAP *ld ATTR_UNUSED, unsigned flags ATTR_UNUSED,
	      void *defaults, void *interact)
{
	struct db_ldap_sasl_bind_context *context = defaults;
	sasl_interact_t *in;
	const char *str;

	for (in = interact; in->id != SASL_CB_LIST_END; in++) {
		switch (in->id) {
		case SASL_CB_GETREALM:
			str = context->realm;
			break;
		case SASL_CB_AUTHNAME:
			str = context->authcid;
			break;
		case SASL_CB_USER:
			str = context->authzid;
			break;
		case SASL_CB_PASS:
			str = context->passwd;
			break;
		default:
			str = NULL;
			break;
		}
		if (str != NULL) {
			in->len = strlen(str);
			in->result = str;
		}
		
	}
	return LDAP_SUCCESS;
}
#endif

static void ldap_connection_timeout(struct ldap_connection *conn)
{
	i_assert(conn->conn_state == LDAP_CONN_STATE_BINDING);

	i_error("LDAP: Initial binding to LDAP server timed out");
	db_ldap_conn_close(conn);
}

static int db_ldap_bind(struct ldap_connection *conn)
{
	int msgid;

	i_assert(conn->conn_state != LDAP_CONN_STATE_BINDING);
	i_assert(conn->default_bind_msgid == -1);
	i_assert(conn->pending_count == 0);

	msgid = ldap_bind(conn->ld, conn->set.dn, conn->set.dnpass,
			  LDAP_AUTH_SIMPLE);
	if (msgid == -1) {
		i_assert(ldap_get_errno(conn) != LDAP_SUCCESS);
		if (db_ldap_connect_finish(conn, ldap_get_errno(conn)) < 0) {
			/* lost connection, close it */
			db_ldap_conn_close(conn);
		}
		return -1;
	}

	conn->conn_state = LDAP_CONN_STATE_BINDING;
	conn->default_bind_msgid = msgid;

	if (conn->to != NULL)
		timeout_remove(&conn->to);
	conn->to = timeout_add(DB_LDAP_REQUEST_LOST_TIMEOUT_SECS*1000,
			       ldap_connection_timeout, conn);
	return 0;
}

static void db_ldap_get_fd(struct ldap_connection *conn)
{
	int ret;

	/* get the connection's fd */
	ret = ldap_get_option(conn->ld, LDAP_OPT_DESC, (void *)&conn->fd);
	if (ret != LDAP_SUCCESS) {
		i_fatal("LDAP: Can't get connection fd: %s",
			ldap_err2string(ret));
	}
	if (conn->fd <= STDERR_FILENO) {
		/* Solaris LDAP library seems to be broken */
		i_fatal("LDAP: Buggy LDAP library returned wrong fd: %d",
			conn->fd);
	}
	i_assert(conn->fd != -1);
	net_set_nonblock(conn->fd, TRUE);
}

static void ATTR_NULL(1)
db_ldap_set_opt(struct ldap_connection *conn, int opt, const void *value,
		const char *optname, const char *value_str)
{
	int ret;

	ret = ldap_set_option(conn == NULL ? NULL : conn->ld, opt, value);
	if (ret != LDAP_SUCCESS) {
		i_fatal("LDAP: Can't set option %s to %s: %s",
			optname, value_str, ldap_err2string(ret));
	}
}

static void ATTR_NULL(1)
db_ldap_set_opt_str(struct ldap_connection *conn, int opt, const char *value,
		    const char *optname)
{
	if (value != NULL)
		db_ldap_set_opt(conn, opt, value, optname, value);
}

static void db_ldap_set_tls_options(struct ldap_connection *conn)
{
	if (!conn->set.tls)
		return;

#ifdef OPENLDAP_TLS_OPTIONS
	db_ldap_set_opt_str(NULL, LDAP_OPT_X_TLS_CACERTFILE,
			    conn->set.tls_ca_cert_file, "tls_ca_cert_file");
	db_ldap_set_opt_str(NULL, LDAP_OPT_X_TLS_CACERTDIR,
			    conn->set.tls_ca_cert_dir, "tls_ca_cert_dir");
	db_ldap_set_opt_str(NULL, LDAP_OPT_X_TLS_CERTFILE,
			    conn->set.tls_cert_file, "tls_cert_file");
	db_ldap_set_opt_str(NULL, LDAP_OPT_X_TLS_KEYFILE,
			    conn->set.tls_key_file, "tls_key_file");
	db_ldap_set_opt_str(NULL, LDAP_OPT_X_TLS_CIPHER_SUITE,
			    conn->set.tls_cipher_suite, "tls_cipher_suite");
	if (conn->set.tls_require_cert != NULL) {
		int value = tls_require_cert2str(conn->set.tls_require_cert);
		db_ldap_set_opt(NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &value,
				"tls_require_cert", conn->set.tls_require_cert);
	}
#else
	if (conn->set.tls_ca_cert_file != NULL ||
	    conn->set.tls_ca_cert_dir != NULL ||
	    conn->set.tls_cert_file != NULL ||
	    conn->set.tls_key_file != NULL ||
	    conn->set.tls_cipher_suite != NULL)
		i_warning("LDAP: tls_* settings ignored, "
			  "your LDAP library doesn't seem to support them");
#endif
}

static void db_ldap_set_options(struct ldap_connection *conn)
{
	unsigned int ldap_version;
	int value;

	db_ldap_set_opt(conn, LDAP_OPT_DEREF, &conn->set.ldap_deref,
			"deref", conn->set.deref);
#ifdef LDAP_OPT_DEBUG_LEVEL
	value = atoi(conn->set.debug_level);
	if (value != 0) {
		db_ldap_set_opt(NULL, LDAP_OPT_DEBUG_LEVEL, &value,
				"debug_level", conn->set.debug_level);
	}
#endif

	if (conn->set.ldap_version < 3) {
		if (conn->set.sasl_bind)
			i_fatal("LDAP: sasl_bind=yes requires ldap_version=3");
		if (conn->set.tls)
			i_fatal("LDAP: tls=yes requires ldap_version=3");
	}

	ldap_version = conn->set.ldap_version;
	db_ldap_set_opt(conn, LDAP_OPT_PROTOCOL_VERSION, &ldap_version,
			"protocol_version", dec2str(ldap_version));
	db_ldap_set_tls_options(conn);
}

int db_ldap_connect(struct ldap_connection *conn)
{
	bool debug = atoi(conn->set.debug_level) > 0;
	struct timeval start, end;
	int ret;

	if (conn->conn_state != LDAP_CONN_STATE_DISCONNECTED)
		return 0;

	if (debug) {
		if (gettimeofday(&start, NULL) < 0)
			memset(&start, 0, sizeof(start));
	}
	i_assert(conn->pending_count == 0);
	if (conn->ld == NULL) {
		if (conn->set.uris != NULL) {
#ifdef LDAP_HAVE_INITIALIZE
			if (ldap_initialize(&conn->ld, conn->set.uris) != LDAP_SUCCESS)
				conn->ld = NULL;
#else
			i_fatal("LDAP: Your LDAP library doesn't support "
				"'uris' setting, use 'hosts' instead.");
#endif
		} else
			conn->ld = ldap_init(conn->set.hosts, LDAP_PORT);

		if (conn->ld == NULL)
			i_fatal("LDAP: ldap_init() failed with hosts: %s",
				conn->set.hosts);

		db_ldap_set_options(conn);
	}

	if (conn->set.tls) {
#ifdef LDAP_HAVE_START_TLS_S
		ret = ldap_start_tls_s(conn->ld, NULL, NULL);
		if (ret != LDAP_SUCCESS) {
			if (ret == LDAP_OPERATIONS_ERROR &&
			    conn->set.uris != NULL &&
			    strncmp(conn->set.uris, "ldaps:", 6) == 0) {
				i_fatal("LDAP: Don't use both tls=yes "
					"and ldaps URI");
			}
			i_error("LDAP: ldap_start_tls_s() failed: %s",
				ldap_err2string(ret));
			return -1;
		}
#else
		i_error("LDAP: Your LDAP library doesn't support TLS");
		return -1;
#endif
	}

	if (conn->set.sasl_bind) {
#ifdef HAVE_LDAP_SASL
		struct db_ldap_sasl_bind_context context;

		memset(&context, 0, sizeof(context));
		context.authcid = conn->set.dn;
		context.passwd = conn->set.dnpass;
		context.realm = conn->set.sasl_realm;
		context.authzid = conn->set.sasl_authz_id;

		/* There doesn't seem to be a way to do SASL binding
		   asynchronously.. */
		ret = ldap_sasl_interactive_bind_s(conn->ld, NULL,
						   conn->set.sasl_mech,
						   NULL, NULL, LDAP_SASL_QUIET,
						   sasl_interact, &context);
		if (db_ldap_connect_finish(conn, ret) < 0)
			return -1;
#else
		i_fatal("LDAP: sasl_bind=yes but no SASL support compiled in");
#endif
		conn->conn_state = LDAP_CONN_STATE_BOUND_DEFAULT;
	} else {
		if (db_ldap_bind(conn) < 0)
			return -1;
	}
	if (debug) {
		if (gettimeofday(&end, NULL) == 0) {
			int msecs = timeval_diff_msecs(&end, &start);
			i_debug("LDAP initialization took %d msecs", msecs);
		}
	}

	db_ldap_get_fd(conn);
	conn->io = io_add(conn->fd, IO_READ, ldap_input, conn);
	return 0;
}

void db_ldap_enable_input(struct ldap_connection *conn, bool enable)
{
	if (!enable) {
		if (conn->io != NULL)
			io_remove(&conn->io);
	} else {
		if (conn->io == NULL && conn->fd != -1) {
			conn->io = io_add(conn->fd, IO_READ, ldap_input, conn);
			ldap_input(conn);
		}
	}
}

static void db_ldap_disconnect_timeout(struct ldap_connection *conn)
{
	db_ldap_abort_requests(conn, UINT_MAX,
		DB_LDAP_REQUEST_DISCONNECT_TIMEOUT_SECS, FALSE,
		"Aborting (timeout), we're not connected to LDAP server");

	if (aqueue_count(conn->request_queue) == 0) {
		/* no requests left, remove this timeout handler */
		timeout_remove(&conn->to);
	}
}

static void db_ldap_conn_close(struct ldap_connection *conn)
{
	struct ldap_request *const *requests, *request;
	unsigned int i;

	conn->conn_state = LDAP_CONN_STATE_DISCONNECTED;
	conn->default_bind_msgid = -1;

	if (conn->to != NULL)
		timeout_remove(&conn->to);

	if (conn->pending_count != 0) {
		requests = array_idx(&conn->request_array, 0);
		for (i = 0; i < conn->pending_count; i++) {
			request = requests[aqueue_idx(conn->request_queue, i)];

			i_assert(request->msgid != -1);
			request->msgid = -1;
		}
		conn->pending_count = 0;
	}

	if (conn->ld != NULL) {
		ldap_unbind(conn->ld);
		conn->ld = NULL;
	}
	conn->fd = -1;

	if (conn->io != NULL) {
		/* the fd may have already been closed before ldap_unbind(),
		   so we'll have to use io_remove_closed(). */
		io_remove_closed(&conn->io);
	}

	if (aqueue_count(conn->request_queue) > 0) {
		conn->to = timeout_add(DB_LDAP_REQUEST_DISCONNECT_TIMEOUT_SECS *
				       1000/2, db_ldap_disconnect_timeout, conn);
	}
}

struct ldap_field_find_context {
	ARRAY_TYPE(string) attr_names;
	pool_t pool;
};

static const char *
db_ldap_field_find(const char *data, void *context)
{
	struct ldap_field_find_context *ctx = context;
	char *ldap_attr;

	if (*data != '\0') {
		ldap_attr = p_strdup(ctx->pool, t_strcut(data, ':'));
		if (strchr(ldap_attr, '@') == NULL)
			array_append(&ctx->attr_names, &ldap_attr, 1);
	}
	return NULL;
}

void db_ldap_set_attrs(struct ldap_connection *conn, const char *attrlist,
		       char ***attr_names_r, ARRAY_TYPE(ldap_field) *attr_map,
		       const char *skip_attr)
{
	static struct var_expand_func_table var_funcs_table[] = {
		{ "ldap", db_ldap_field_find },
		{ "ldap_ptr", db_ldap_field_find },
		{ NULL, NULL }
	};
	struct ldap_field_find_context ctx;
	struct ldap_field *field;
	string_t *tmp_str;
	const char *const *attr, *attr_data, *p;
	char *ldap_attr, *name, *templ;
	unsigned int i;

	if (*attrlist == '\0')
		return;

	attr = t_strsplit_spaces(attrlist, ",");

	tmp_str = t_str_new(128);
	ctx.pool = conn->pool;
	p_array_init(&ctx.attr_names, conn->pool, 16);
	for (i = 0; attr[i] != NULL; i++) {
		/* allow spaces here so "foo=1, bar=2" works */
		attr_data = attr[i];
		while (*attr_data == ' ') attr_data++;

		p = strchr(attr_data, '=');
		if (p == NULL)
			ldap_attr = name = p_strdup(conn->pool, attr_data);
		else if (attr_data[0] == '@') {
			ldap_attr = "";
			name = p_strdup(conn->pool, attr_data);
		} else {
			ldap_attr = p_strdup_until(conn->pool, attr_data, p);
			name = p_strdup(conn->pool, p + 1);
		}

		templ = strchr(name, '=');
		if (templ == NULL) {
			if (*ldap_attr == '\0') {
				/* =foo static value */
				templ = "";
			}
		} else {
			*templ++ = '\0';
			str_truncate(tmp_str, 0);
			var_expand_with_funcs(tmp_str, templ, NULL,
					      var_funcs_table, &ctx);
			if (strchr(templ, '%') == NULL) {
				/* backwards compatibility:
				   attr=name=prefix means same as
				   attr=name=prefix%$ when %vars are missing */
				templ = p_strconcat(conn->pool, templ,
						    "%$", NULL);
			}
		}

		if (*name == '\0')
			i_error("ldap: Invalid attrs entry: %s", attr_data);
		else if (skip_attr == NULL || strcmp(skip_attr, name) != 0) {
			field = array_append_space(attr_map);
			if (name[0] == '@') {
				/* @name=ldapField */
				name++;
				field->value_is_dn = TRUE;
			} else if (name[0] == '!' && name == ldap_attr) {
				/* !ldapAttr */
				name = "";
				ldap_attr++;
				field->skip = TRUE;
			}
			field->name = name;
			field->value = templ;
			field->ldap_attr_name = ldap_attr;
			if (*ldap_attr != '\0' &&
			    strchr(ldap_attr, '@') == NULL) {
				/* root request's attribute */
				array_append(&ctx.attr_names, &ldap_attr, 1);
			}
		}
	}
	array_append_zero(&ctx.attr_names);
	*attr_names_r = array_idx_modifiable(&ctx.attr_names, 0);
}

static const struct var_expand_table *
db_ldap_value_get_var_expand_table(struct auth_request *auth_request,
				   const char *ldap_value)
{
	struct var_expand_table *table;
	unsigned int count = 1;

	table = auth_request_get_var_expand_table_full(auth_request, NULL,
						       &count);
	table[0].key = '$';
	table[0].value = ldap_value;
	return table;
}

#define IS_LDAP_ESCAPED_CHAR(c) \
	((c) == '*' || (c) == '(' || (c) == ')' || (c) == '\\')

const char *ldap_escape(const char *str,
			const struct auth_request *auth_request ATTR_UNUSED)
{
	const char *p;
	string_t *ret;

	for (p = str; *p != '\0'; p++) {
		if (IS_LDAP_ESCAPED_CHAR(*p))
			break;
	}

	if (*p == '\0')
		return str;

	ret = t_str_new((size_t) (p - str) + 64);
	str_append_n(ret, str, (size_t) (p - str));

	for (; *p != '\0'; p++) {
		if (IS_LDAP_ESCAPED_CHAR(*p))
			str_append_c(ret, '\\');
		str_append_c(ret, *p);
	}
	return str_c(ret);
}

static bool
ldap_field_hide_password(struct db_ldap_result_iterate_context *ctx,
			 const char *attr)
{
	const struct ldap_field *field;

	if (ctx->auth_request->set->debug_passwords)
		return FALSE;

	array_foreach(ctx->attr_map, field) {
		if (strcmp(field->ldap_attr_name, attr) == 0) {
			if (strcmp(field->name, "password") == 0 ||
			    strcmp(field->name, "password_noscheme") == 0)
				return TRUE;
		}
	}
	return FALSE;
}

static void
get_ldap_fields(struct db_ldap_result_iterate_context *ctx,
		struct ldap_connection *conn, LDAPMessage *entry,
		const char *suffix)
{
	struct db_ldap_value *ldap_value;
	char *attr, **vals;
	unsigned int i, count;
	BerElement *ber;

	attr = ldap_first_attribute(conn->ld, entry, &ber);
	while (attr != NULL) {
		vals = ldap_get_values(conn->ld, entry, attr);

		ldap_value = p_new(ctx->pool, struct db_ldap_value, 1);
		if (vals == NULL) {
			ldap_value->values = p_new(ctx->pool, const char *, 1);
			count = 0;
		} else {
			for (count = 0; vals[count] != NULL; count++) ;
		}

		ldap_value->values = p_new(ctx->pool, const char *, count + 1);
		for (i = 0; i < count; i++)
			ldap_value->values[i] = p_strdup(ctx->pool, vals[i]);

		if (ctx->debug != NULL) {
			str_printfa(ctx->debug, " %s%s=", attr, suffix);
			if (count == 0)
				str_append(ctx->debug, "<no values>");
			else if (ldap_field_hide_password(ctx, attr))
				str_append(ctx->debug, PASSWORD_HIDDEN_STR);
			else {
				str_append(ctx->debug, ldap_value->values[0]);
				for (i = 1; i < count; i++) {
					str_printfa(ctx->debug, ",%s",
						    ldap_value->values[0]);
				}
			}
		}
		hash_table_insert(ctx->ldap_attrs,
				  p_strconcat(ctx->pool, attr, suffix, NULL),
				  ldap_value);

		ldap_value_free(vals);
		ldap_memfree(attr);
		attr = ldap_next_attribute(conn->ld, entry, ber);
	}
	ber_free(ber, 0);
}

struct db_ldap_result_iterate_context *
db_ldap_result_iterate_init_full(struct ldap_connection *conn,
				 struct ldap_request_search *ldap_request,
				 LDAPMessage *res, bool skip_null_values,
				 bool iter_dn_values)
{
	struct db_ldap_result_iterate_context *ctx;
	const struct ldap_request_named_result *named_res;
	const char *suffix;
	pool_t pool;

	pool = pool_alloconly_create(MEMPOOL_GROWING"ldap result iter", 1024);
	ctx = p_new(pool, struct db_ldap_result_iterate_context, 1);
	ctx->pool = pool;
	ctx->auth_request = ldap_request->request.auth_request;
	ctx->attr_map = ldap_request->attr_map;
	ctx->skip_null_values = skip_null_values;
	ctx->iter_dn_values = iter_dn_values;
	hash_table_create(&ctx->ldap_attrs, pool, 0, strcase_hash, strcasecmp);
	if (ctx->auth_request->set->debug)
		ctx->debug = t_str_new(256);

	get_ldap_fields(ctx, conn, res, "");
	if (array_is_created(&ldap_request->named_results)) {
		array_foreach(&ldap_request->named_results, named_res) {
			suffix = t_strdup_printf("@%s", named_res->field->name);
			if (named_res->result != NULL) {
				get_ldap_fields(ctx, conn,
						named_res->result->msg, suffix);
			}
		}
	}
	return ctx;
}

struct db_ldap_result_iterate_context *
db_ldap_result_iterate_init(struct ldap_connection *conn,
			    struct ldap_request_search *ldap_request,
			    LDAPMessage *res, bool skip_null_values)
{
	return db_ldap_result_iterate_init_full(conn, ldap_request, res,
						skip_null_values, FALSE);
}

static const char *db_ldap_field_get_default(const char *data)
{
	const char *p;

	p = strchr(data, ':');
	if (p == NULL)
		return "";
	else {
		/* default value given */
		return p+1;
	}
}

static const char *db_ldap_field_expand(const char *data, void *context)
{
	struct db_ldap_result_iterate_context *ctx = context;
	struct db_ldap_value *ldap_value;
	const char *field_name = t_strcut(data, ':');

	ldap_value = hash_table_lookup(ctx->ldap_attrs, field_name);
	if (ldap_value == NULL) {
		/* requested ldap attribute wasn't returned at all */
		if (ctx->debug)
			str_printfa(ctx->debug, "; %s missing", field_name);
		return db_ldap_field_get_default(data);
	}
	ldap_value->used = TRUE;

	if (ldap_value->values[0] == NULL) {
		/* no value for ldap attribute */
		return db_ldap_field_get_default(data);
	}
	if (ldap_value->values[1] != NULL) {
		auth_request_log_warning(ctx->auth_request, AUTH_SUBSYS_DB,
			"Multiple values found for '%s', using value '%s'",
			field_name, ldap_value->values[0]);
	}
	return ldap_value->values[0];
}

static const char *db_ldap_field_ptr_expand(const char *data, void *context)
{
	struct db_ldap_result_iterate_context *ctx = context;
	const char *field_name, *suffix;

	suffix = strchr(t_strcut(data, ':'), '@');
	field_name = db_ldap_field_expand(data, ctx);
	if (field_name[0] == '\0')
		return "";
	field_name = t_strconcat(field_name, suffix, NULL);
	return db_ldap_field_expand(field_name, ctx);
}

static const char *const *
db_ldap_result_return_value(struct db_ldap_result_iterate_context *ctx,
			    const struct ldap_field *field,
			    struct db_ldap_value *ldap_value)
{
	static struct var_expand_func_table var_funcs_table[] = {
		{ "ldap", db_ldap_field_expand },
		{ "ldap_ptr", db_ldap_field_ptr_expand },
		{ NULL, NULL }
	};
	const struct var_expand_table *var_table;
	const char *const *values;

	if (ldap_value != NULL)
		values = ldap_value->values;
	else {
		/* LDAP attribute doesn't exist */
		ctx->val_1_arr[0] = NULL;
		values = ctx->val_1_arr;
	}

	if (field->value == NULL) {
		/* use the LDAP attribute's value */
	} else {
		/* template */
		if (values[0] == NULL && *field->ldap_attr_name != '\0') {
			/* ldapAttr=key=template%$, but ldapAttr doesn't
			   exist. */
			return values;
		}
		if (values[0] != NULL && values[1] != NULL) {
			auth_request_log_warning(ctx->auth_request, AUTH_SUBSYS_DB,
				"Multiple values found for '%s', "
				"using value '%s'",
				field->name, values[0]);
		}

		/* do this lookup separately for each expansion, because:
		   1) the values are allocated from data stack
		   2) if "user" field is updated, we want %u/%n/%d updated
		      (and less importantly the same for other variables) */
		var_table = db_ldap_value_get_var_expand_table(ctx->auth_request,
							       values[0]);
		if (ctx->var == NULL)
			ctx->var = str_new(ctx->pool, 256);
		else
			str_truncate(ctx->var, 0);
		var_expand_with_funcs(ctx->var, field->value, var_table,
				      var_funcs_table, ctx);
		ctx->val_1_arr[0] = str_c(ctx->var);
		values = ctx->val_1_arr;
	}
	return values;
}

bool db_ldap_result_iterate_next(struct db_ldap_result_iterate_context *ctx,
				 const char **name_r,
				 const char *const **values_r)
{
	const struct ldap_field *field;
	struct db_ldap_value *ldap_value;

	do {
		if (ctx->attr_idx == array_count(ctx->attr_map))
			return FALSE;
		field = array_idx(ctx->attr_map, ctx->attr_idx++);
	} while (field->value_is_dn != ctx->iter_dn_values ||
		 field->skip);

	ldap_value = *field->ldap_attr_name == '\0' ? NULL :
		hash_table_lookup(ctx->ldap_attrs, field->ldap_attr_name);
	if (ldap_value != NULL)
		ldap_value->used = TRUE;
	else if (ctx->debug && *field->ldap_attr_name != '\0')
		str_printfa(ctx->debug, "; %s missing", field->ldap_attr_name);

	*name_r = field->name;
	*values_r = db_ldap_result_return_value(ctx, field, ldap_value);

	if (ctx->skip_null_values && (*values_r)[0] == NULL) {
		/* no values. don't confuse the caller with this reply. */
		return db_ldap_result_iterate_next(ctx, name_r, values_r);
	}
	return TRUE;
}

static void
db_ldap_result_finish_debug(struct db_ldap_result_iterate_context *ctx)
{
	struct hash_iterate_context *iter;
	char *name;
	struct db_ldap_value *value;
	unsigned int orig_len, unused_count = 0;

	orig_len = str_len(ctx->debug);
	if (orig_len == 0) {
		auth_request_log_debug(ctx->auth_request, AUTH_SUBSYS_DB,
				       "no fields returned by the server");
		return;
	}

	str_append(ctx->debug, "; ");

	iter = hash_table_iterate_init(ctx->ldap_attrs);
	while (hash_table_iterate(iter, ctx->ldap_attrs, &name, &value)) {
		if (!value->used) {
			str_printfa(ctx->debug, "%s,", name);
			unused_count++;
		}
	}
	hash_table_iterate_deinit(&iter);

	if (unused_count == 0)
		str_truncate(ctx->debug, orig_len);
	else {
		str_truncate(ctx->debug, str_len(ctx->debug)-1);
		str_append(ctx->debug, " unused");
	}
	auth_request_log_debug(ctx->auth_request, AUTH_SUBSYS_DB,
			       "result: %s", str_c(ctx->debug) + 1);
}

void db_ldap_result_iterate_deinit(struct db_ldap_result_iterate_context **_ctx)
{
	struct db_ldap_result_iterate_context *ctx = *_ctx;

	*_ctx = NULL;

	if (ctx->debug != NULL)
		db_ldap_result_finish_debug(ctx);
	hash_table_destroy(&ctx->ldap_attrs);
	pool_unref(&ctx->pool);
}

static const char *parse_setting(const char *key, const char *value,
				 struct ldap_connection *conn)
{
	return parse_setting_from_defs(conn->pool, setting_defs,
				       &conn->set, key, value);
}

static struct ldap_connection *ldap_conn_find(const char *config_path)
{
	struct ldap_connection *conn;

	for (conn = ldap_connections; conn != NULL; conn = conn->next) {
		if (strcmp(conn->config_path, config_path) == 0)
			return conn;
	}

	return NULL;
}

struct ldap_connection *db_ldap_init(const char *config_path, bool userdb)
{
	struct ldap_connection *conn;
	const char *str, *error;
	pool_t pool;

	/* see if it already exists */
	conn = ldap_conn_find(config_path);
	if (conn != NULL) {
		if (userdb)
			conn->userdb_used = TRUE;
		conn->refcount++;
		return conn;
	}

	if (*config_path == '\0')
		i_fatal("LDAP: Configuration file path not given");

	pool = pool_alloconly_create("ldap_connection", 1024);
	conn = p_new(pool, struct ldap_connection, 1);
	conn->pool = pool;
	conn->refcount = 1;

	conn->userdb_used = userdb;
	conn->conn_state = LDAP_CONN_STATE_DISCONNECTED;
	conn->default_bind_msgid = -1;
	conn->fd = -1;
	conn->config_path = p_strdup(pool, config_path);
	conn->set = default_ldap_settings;
	if (!settings_read_nosection(config_path, parse_setting, conn, &error))
		i_fatal("ldap %s: %s", config_path, error);

	if (conn->set.base == NULL)
		i_fatal("LDAP: No base given");

	if (conn->set.uris == NULL && conn->set.hosts == NULL)
		i_fatal("LDAP: No uris or hosts set");
#ifndef LDAP_HAVE_INITIALIZE
	if (conn->set.uris != NULL) {
		i_fatal("LDAP: Dovecot compiled without support for LDAP uris "
			"(ldap_initialize not found)");
	}
#endif

	if (*conn->set.ldaprc_path != '\0') {
		str = getenv("LDAPRC");
		if (str != NULL && strcmp(str, conn->set.ldaprc_path) != 0) {
			i_fatal("LDAP: Multiple different ldaprc_path "
				"settings not allowed (%s and %s)",
				str, conn->set.ldaprc_path);
		}
		env_put(t_strconcat("LDAPRC=", conn->set.ldaprc_path, NULL));
	}

        conn->set.ldap_deref = deref2str(conn->set.deref);
	conn->set.ldap_scope = scope2str(conn->set.scope);

	i_array_init(&conn->request_array, 512);
	conn->request_queue = aqueue_init(&conn->request_array.arr);

	conn->next = ldap_connections;
        ldap_connections = conn;
	return conn;
}

void db_ldap_unref(struct ldap_connection **_conn)
{
        struct ldap_connection *conn = *_conn;
	struct ldap_connection **p;

	*_conn = NULL;
	i_assert(conn->refcount >= 0);
	if (--conn->refcount > 0)
		return;

	for (p = &ldap_connections; *p != NULL; p = &(*p)->next) {
		if (*p == conn) {
			*p = conn->next;
			break;
		}
	}

	db_ldap_abort_requests(conn, UINT_MAX, 0, FALSE, "Shutting down");
	i_assert(conn->pending_count == 0);
	db_ldap_conn_close(conn);
	i_assert(conn->to == NULL);

	array_free(&conn->request_array);
	aqueue_deinit(&conn->request_queue);

	pool_unref(&conn->pool);
}

#ifndef BUILTIN_LDAP
/* Building a plugin */
extern struct passdb_module_interface passdb_ldap_plugin;
extern struct userdb_module_interface userdb_ldap_plugin;

void authdb_ldap_init(void);
void authdb_ldap_deinit(void);

void authdb_ldap_init(void)
{
	passdb_register_module(&passdb_ldap_plugin);
	userdb_register_module(&userdb_ldap_plugin);

}
void authdb_ldap_deinit(void)
{
	passdb_unregister_module(&passdb_ldap_plugin);
	userdb_unregister_module(&userdb_ldap_plugin);
}
#endif

#endif
