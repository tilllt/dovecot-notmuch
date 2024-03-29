/* Copyright (c) 2013-2014 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "strfuncs.h"
#include "net.h"
#include "uri-util.h"

#include "http-url.h"
#include "http-request.h"

/*
 * HTTP URL parser
 */

struct http_url_parser {
	struct uri_parser parser;

	enum http_url_parse_flags flags;

	struct http_url *url;
	struct http_url *base;

	enum http_request_target_format req_format;

 	unsigned int relative:1;
	unsigned int request_target:1;
};

static bool http_url_parse_authority(struct http_url_parser *url_parser)
{
	struct uri_parser *parser = &url_parser->parser;
	struct http_url *url = url_parser->url;
	struct uri_authority auth;
	const char *user = NULL, *password = NULL;
	int ret;

	if ((ret = uri_parse_authority(parser, &auth)) < 0)
		return FALSE;
	if (ret > 0) {
		if (auth.enc_userinfo != NULL) {
			const char *p;

			if ((url_parser->flags & HTTP_URL_ALLOW_USERINFO_PART) == 0) {
				/* RFC 7230, Section 2.7.1: http URI Scheme

				   A sender MUST NOT generate the userinfo subcomponent (and its "@"
				   delimiter) when an "http" URI reference is generated within a
				   message as a request target or header field value.  Before making
				   use of an "http" URI reference received from an untrusted source,
				   a recipient SHOULD parse for userinfo and treat its presence as
				   an error; it is likely being used to obscure the authority for
				   the sake of phishing attacks.
				 */
				parser->error = "HTTP URL does not allow `userinfo@' part";
				return FALSE;
			}

			p = strchr(auth.enc_userinfo, ':');
			if (p == NULL) {
				if (!uri_data_decode(parser, auth.enc_userinfo, NULL, &user))
					return FALSE;
			} else {
				if (!uri_data_decode(parser, auth.enc_userinfo, p, &user))
					return FALSE;
				if (!uri_data_decode(parser, p+1, NULL, &password))
					return FALSE;
			}
		}
	}
	if (url != NULL) {
		url->host_name = p_strdup(parser->pool, auth.host_literal);
		url->host_ip = auth.host_ip;
		url->have_host_ip = auth.have_host_ip;
		url->port = auth.port;
		url->have_port = auth.have_port;
		url->user = p_strdup(parser->pool, user);
		url->password = p_strdup(parser->pool, password);
	}
	return TRUE;
}

static bool http_url_parse_authority_form(struct http_url_parser *url_parser)
{
	struct uri_parser *parser = &url_parser->parser;

	if (!http_url_parse_authority(url_parser))
		return FALSE;
	if (parser->cur != parser->end)
		return FALSE;
	url_parser->req_format = HTTP_REQUEST_TARGET_FORMAT_AUTHORITY;
	return TRUE;
}

static bool http_url_do_parse(struct http_url_parser *url_parser)
{
	struct uri_parser *parser = &url_parser->parser;
	struct http_url *url = url_parser->url, *base = url_parser->base;
	const char *const *path;
	bool relative = TRUE, have_scheme = FALSE, have_authority = FALSE,
		have_path = FALSE;
	int path_relative;
	const char *part;
	int ret;

	/* RFC 7230, Appendix B:

	   http-URI       = "http://" authority path-abempty [ "?" query ]
	                    [ "#" fragment ]
	   https-URI      = "https://" authority path-abempty [ "?" query ]
	                    [ "#" fragment ]
	   partial-URI    = relative-part [ "?" query ]

	   request-target = origin-form / absolute-form / authority-form /
	                    asterisk-form

	   origin-form    = absolute-path [ "?" query ]
	   absolute-form  = absolute-URI
	   authority-form = authority
	   asterisk-form  = "*"
	                  ; Not parsed here

	   absolute-path  = 1*( "/" segment )

	   RFC 3986, Appendix A: (implemented in uri-util.h)

	   absolute-URI   = scheme ":" hier-part [ "?" query ]

	   hier-part      = "//" authority path-abempty
	                  / path-absolute
	                  / path-rootless
	                  / path-empty

	   relative-part  = "//" authority path-abempty
	                  / path-absolute
	                  / path-noscheme
	                  / path-empty

	   authority     = [ userinfo "@" ] host [ ":" port ]

	   path-abempty   = *( "/" segment )
	   path-absolute  = "/" [ segment-nz *( "/" segment ) ]
	   path-noscheme  = segment-nz-nc *( "/" segment )
	   path-rootless  = segment-nz *( "/" segment )
	   path-empty     = 0<pchar>

	   segment        = *pchar
	   segment-nz     = 1*pchar
	   segment-nz-nc  = 1*( unreserved / pct-encoded / sub-delims / "@" )
                    ; non-zero-length segment without any colon ":"

	   query          = *( pchar / "/" / "?" )
	   fragment       = *( pchar / "/" / "?" )
	 */

	/* "http:" / "https:" */
	if ((url_parser->flags & HTTP_URL_PARSE_SCHEME_EXTERNAL) == 0) {
		const char *scheme;

		if ((ret = uri_parse_scheme(parser, &scheme)) < 0)
			return FALSE;
		else if (ret > 0) {
			if (strcasecmp(scheme, "https") == 0) {
				if (url != NULL)
					url->have_ssl = TRUE;
			} else if (strcasecmp(scheme, "http") != 0) {
				if (url_parser->request_target) {
					/* valid as non-HTTP scheme, but also try to parse as authority */
					parser->cur = parser->begin;
					if (!http_url_parse_authority_form(url_parser)) {
						url_parser->url = NULL; /* indicate non-http-url */
						url_parser->req_format = HTTP_REQUEST_TARGET_FORMAT_ABSOLUTE;
					}
					return TRUE;
				}
				parser->error = "Not an HTTP URL";
				return FALSE;
			}
			relative = FALSE;
			have_scheme = TRUE;
		}
	} else {
		relative = FALSE;
		have_scheme = TRUE;
	}

	/* "//" authority   ; or
	 * ["//"] authority ; when parsing a request target
	 */
	if (parser->cur < parser->end && parser->cur[0] == '/') {
		if (parser->cur+1 < parser->end && parser->cur[1] == '/') {
			parser->cur += 2;
			relative = FALSE;
			have_authority = TRUE;
		} else {
			/* start of absolute-path */
		}
	} else if (url_parser->request_target && !have_scheme) {
		if (!http_url_parse_authority_form(url_parser)) {
			/* not non-HTTP scheme and invalid as authority-form */
			parser->error = "Request target is invalid";
			return FALSE;
		}
		return TRUE;
	}

	if (have_scheme && !have_authority) {
		parser->error = "Absolute HTTP URL requires `//' after `http:'";
 		return FALSE;
	}

	if (have_authority) {
		if (!http_url_parse_authority(url_parser))
			return FALSE;
	}

	/* path-abempty / path-absolute / path-noscheme / path-empty */
	if ((ret = uri_parse_path(parser, &path_relative, &path)) < 0)
		return FALSE;

	/* Relative URLs are only valid when we have a base URL */
	if (relative) {
		if (base == NULL) {
			parser->error = "Relative HTTP URL not allowed";
			return FALSE;
		} else if (!have_authority && url != NULL) {
			url->host_name = p_strdup_empty(parser->pool, base->host_name); 
			url->host_ip = base->host_ip;
			url->have_host_ip = base->have_host_ip;
			url->port = base->port;
			url->have_port = base->have_port;
			url->have_ssl = base->have_ssl;
			url->user = p_strdup_empty(parser->pool, base->user);
			url->password = p_strdup_empty(parser->pool, base->password);
		}

		url_parser->relative = TRUE;
	}

	/* Resolve path */
	if (ret > 0) {
		string_t *fullpath = NULL;

		have_path = TRUE;

		if (url != NULL)
			fullpath = t_str_new(256);

		if (relative && path_relative > 0 && base->path != NULL) {
			const char *pbegin = base->path;
			const char *pend = base->path + strlen(base->path);
			const char *p = pend - 1;

			i_assert(*pbegin == '/');

			/* discard trailing segments of base path based on how many effective
			   leading '..' segments were found in the relative path.
			 */
			while (path_relative > 0 && p > pbegin) {
				while (p > pbegin && *p != '/') p--;
				if (p >= pbegin) {
					pend = p;
					path_relative--;
				}
				if (p > pbegin) p--;
			}

			if (url != NULL && pend > pbegin)
				str_append_n(fullpath, pbegin, pend-pbegin);
		}

		/* append relative path */
		while (*path != NULL) {
			if (!uri_data_decode(parser, *path, NULL, &part))
				return FALSE;

			if (url != NULL) {
				str_append_c(fullpath, '/');
				str_append(fullpath, part);
			}
			path++;
		}

		if (url != NULL)
			url->path = p_strdup(parser->pool, str_c(fullpath));
	} else if (relative && url != NULL) {
		url->path = p_strdup(parser->pool, base->path);
	}

	/* [ "?" query ] */
	if ((ret = uri_parse_query(parser, &part)) < 0)
		return FALSE;
	if (ret > 0) {
		if (!uri_data_decode(parser, part, NULL, NULL)) // check only
			return FALSE;
		if (url != NULL)
			url->enc_query = p_strdup(parser->pool, part);
	} else if (relative && !have_path && url != NULL) {
		url->enc_query = p_strdup(parser->pool, base->enc_query);
	}

	/* [ "#" fragment ] */
	if ((ret = uri_parse_fragment(parser, &part)) < 0)
		return FALSE;
	if (ret > 0) {
		if ((url_parser->flags & HTTP_URL_ALLOW_FRAGMENT_PART) == 0) {
			parser->error = "URL fragment not allowed for HTTP URL in this context";
			return FALSE;
		}
		if (!uri_data_decode(parser, part, NULL, NULL)) // check only
			return FALSE;
		if (url != NULL)
			url->enc_fragment =  p_strdup(parser->pool, part);
	} else if (relative && !have_path && url != NULL) {
		url->enc_fragment = p_strdup(parser->pool, base->enc_fragment);
	}

	if (parser->cur != parser->end) {
		parser->error = "HTTP URL contains invalid character";
		return FALSE;
	}

	if (have_scheme)
		url_parser->req_format = HTTP_REQUEST_TARGET_FORMAT_ABSOLUTE;
	return TRUE;
}

/* Public API */

int http_url_parse(const char *url, struct http_url *base,
		   enum http_url_parse_flags flags, pool_t pool,
		   struct http_url **url_r, const char **error_r)
{
	struct http_url_parser url_parser;

	/* base != NULL indicates whether relative URLs are allowed. However, certain
	   flags may also dictate whether relative URLs are allowed/required. */
	i_assert((flags & HTTP_URL_PARSE_SCHEME_EXTERNAL) == 0 || base == NULL);

	memset(&url_parser, '\0', sizeof(url_parser));
	uri_parser_init(&url_parser.parser, pool, url);

	url_parser.url = p_new(pool, struct http_url, 1);
	url_parser.base = base;
	url_parser.flags = flags;

	if (!http_url_do_parse(&url_parser)) {
		*error_r = url_parser.parser.error;
		return -1;
	}
	*url_r = url_parser.url;
	return 0;
}

int http_url_request_target_parse(const char *request_target,
	const char *host_header, pool_t pool, struct http_request_target *target,
	const char **error_r)
{
	struct http_url_parser url_parser;
	struct uri_parser *parser;
	struct uri_authority host;
	struct http_url base;

	memset(&url_parser, '\0', sizeof(url_parser));
	parser = &url_parser.parser;
	uri_parser_init(parser, pool, host_header);

	if (uri_parse_authority(parser, &host) <= 0) {
		*error_r = t_strdup_printf("Invalid Host header: %s", parser->error);
		return -1;
	}

	if (parser->cur != parser->end || host.enc_userinfo != NULL) {
		*error_r = "Invalid Host header: Contains invalid character";
		return -1;
	}

	if (request_target[0] == '*' && request_target[1] == '\0') {
		struct http_url *url = p_new(pool, struct http_url, 1);
		url->host_name = p_strdup(pool, host.host_literal);
		url->host_ip = host.host_ip;
		url->port = host.port;
		url->have_host_ip = host.have_host_ip;
		url->have_port = host.have_port;
		target->url = url;
		target->format = HTTP_REQUEST_TARGET_FORMAT_ASTERISK;
		return 0;
	}

	memset(&base, 0, sizeof(base));
	base.host_name = host.host_literal;
	base.host_ip = host.host_ip;
	base.port = host.port;
	base.have_host_ip = host.have_host_ip;
	base.have_port = host.have_port;

	memset(parser, '\0', sizeof(*parser));
	uri_parser_init(parser, pool, request_target);

	url_parser.url = p_new(pool, struct http_url, 1);
	url_parser.request_target = TRUE;
	url_parser.req_format = HTTP_REQUEST_TARGET_FORMAT_ORIGIN;
	url_parser.base = &base;
	url_parser.flags = 0;

	if (!http_url_do_parse(&url_parser)) {
		*error_r = url_parser.parser.error;
		return -1;
	}

	target->url = url_parser.url;
	target->format = url_parser.req_format;
	return 0;
}

/*
 * HTTP URL manipulation
 */

void http_url_copy_authority(pool_t pool, struct http_url *dest,
	const struct http_url *src)
{
	dest->host_name = p_strdup(pool, src->host_name);
	if (src->have_host_ip) {
		dest->host_ip = src->host_ip;
		dest->have_host_ip = TRUE;
	}
	if (src->have_port) {
		dest->port = src->port;
		dest->have_port = TRUE;
	}
	dest->have_ssl = src->have_ssl;
}

void http_url_copy(pool_t pool, struct http_url *dest,
	const struct http_url *src)
{
	http_url_copy_authority(pool, dest, src);
	dest->path = p_strdup(pool, src->path);
	dest->enc_query = p_strdup(pool, src->enc_query);
	dest->enc_fragment = p_strdup(pool, src->enc_fragment);
}

struct http_url *http_url_clone(pool_t pool,const struct http_url *src)
{
	struct http_url *new_url;

	new_url = p_new(pool, struct http_url, 1);
	http_url_copy(pool, new_url, src);

	return new_url;
}

/*
 * HTTP URL construction
 */

static void
http_url_add_scheme(string_t *urlstr, const struct http_url *url)
{
	/* scheme */
	if (!url->have_ssl)
		uri_append_scheme(urlstr, "http");
	else
		uri_append_scheme(urlstr, "https");
	str_append(urlstr, "//");
}

static void
http_url_add_authority(string_t *urlstr, const struct http_url *url)
{
	/* host:port */
	if (url->host_name != NULL) {
		/* assume IPv6 literal if starts with '['; avoid encoding */
		if (*url->host_name == '[')
			str_append(urlstr, url->host_name);
		else
			uri_append_host_name(urlstr, url->host_name);
	} else if (url->have_host_ip) {
		uri_append_host_ip(urlstr, &url->host_ip);
	} else
		i_unreached();
	if (url->have_port)
		uri_append_port(urlstr, url->port);
}

static void
http_url_add_target(string_t *urlstr, const struct http_url *url)
{
	if (url->path == NULL || *url->path == '\0') {
		/* Older syntax of RFC 2616 requires this slash at all times for an
			 absolute URL
		 */
		str_append_c(urlstr, '/');
	} else {
		uri_append_path_data(urlstr, "", url->path);
	}

	/* query (pre-encoded) */
	if (url->enc_query != NULL) {
		str_append_c(urlstr, '?');
		str_append(urlstr, url->enc_query);
	}
}

const char *http_url_create(const struct http_url *url)
{
	string_t *urlstr = t_str_new(512);

	http_url_add_scheme(urlstr, url);
	http_url_add_authority(urlstr, url);
	http_url_add_target(urlstr, url);

	/* fragment */
	if (url->enc_fragment != NULL) {
		str_append_c(urlstr, '#');
		str_append(urlstr, url->enc_fragment);
	}

	return str_c(urlstr);
}

const char *http_url_create_host(const struct http_url *url)
{
	string_t *urlstr = t_str_new(512);

	http_url_add_scheme(urlstr, url);
	http_url_add_authority(urlstr, url);

	return str_c(urlstr);
}

const char *http_url_create_authority(const struct http_url *url)
{
	string_t *urlstr = t_str_new(256);

	http_url_add_authority(urlstr, url);

	return str_c(urlstr);
}

const char *http_url_create_target(const struct http_url *url)
{
	string_t *urlstr = t_str_new(256);

	http_url_add_target(urlstr, url);

	return str_c(urlstr);
}

void http_url_escape_path(string_t *out, const char *data)
{
	uri_append_query_data(out, "&;?=+", data);
}

void http_url_escape_param(string_t *out, const char *data)
{
	uri_append_query_data(out, "&;/?=+", data);
}
