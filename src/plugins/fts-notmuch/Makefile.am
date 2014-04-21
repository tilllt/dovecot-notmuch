AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/plugins/fts

NOPLUGIN_LDFLAGS =
lib21_fts_notmuch_plugin_la_LDFLAGS = -module -avoid-version

module_LTLIBRARIES = \
	lib21_fts_notmuch_plugin.la

if DOVECOT_PLUGIN_DEPS
fts_plugin_dep = ../fts/lib20_fts_plugin.la
endif

lib21_fts_notmuch_plugin_la_LIBADD = \
	$(fts_plugin_dep) \
	-lnotmuch

lib21_fts_notmuch_plugin_la_SOURCES = \
	fts-backend-notmuch.c \
	fts-notmuch-plugin.c

noinst_HEADERS = \
	fts-notmuch-plugin.h
