# dovecot.m4 - Check presence of dovecot -*-Autoconf-*-
#
#   Copyright (C) 2010 Dennis Schridde
#
# This file is free software; the authors give
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.

# serial 13

AC_DEFUN([DC_DOVECOT_MODULEDIR],[
	AC_ARG_WITH(moduledir,
	[  --with-moduledir=DIR    Base directory for dynamically loadable modules],
		moduledir="$withval",
		moduledir=$libdir/dovecot
	)
	AC_SUBST(moduledir)
])

AC_DEFUN([DC_PLUGIN_DEPS],[
	_plugin_deps=yes
	AC_MSG_CHECKING([whether OS supports plugin dependencies])
	case "$host_os" in
	  darwin*)
	    # OSX loads the plugins twice, which breaks stuff
	    _plugin_deps=no
	    ;;
	esac
	AC_MSG_RESULT([$_plugin_deps])
	AM_CONDITIONAL([DOVECOT_PLUGIN_DEPS], [test "x$_plugin_deps" = "xyes"])
	unset _plugin_deps
])

# Substitute every var in the given comma seperated list
AC_DEFUN([AX_SUBST_L],[
	m4_foreach([__var__], [$@], [AC_SUBST(__var__)])
])

AC_DEFUN([DC_DOVECOT],[
	AC_ARG_WITH(dovecot,
	  [  --with-dovecot=DIR      Dovecot base directory],
			[ dovecotdir="$withval" ], [
			  dc_prefix=$prefix
			  test "x$dc_prefix" = xNONE && dc_prefix=$ac_default_prefix
			  dovecotdir="$dc_prefix/lib/dovecot"
			]
	)

	AC_ARG_WITH(dovecot-install-dirs,
		[AC_HELP_STRING([--with-dovecot-install-dirs],
		[Use install directories configured for Dovecot (default)])],
	if test x$withval = xno; then
		use_install_dirs=no
	else
		use_install_dirs=yes
	fi,
	use_install_dirs=yes)

	AC_MSG_CHECKING([for "$dovecotdir/dovecot-config"])
	if test -f "$dovecotdir/dovecot-config"; then
		AC_MSG_RESULT([$dovecotdir/dovecot-config])
	else
		AC_MSG_RESULT([not found])
		AC_MSG_NOTICE([])
		AC_MSG_NOTICE([Use --with-dovecot=DIR to provide the path to the dovecot-config file.])
		AC_MSG_ERROR([dovecot-config not found])
	fi

	old=`pwd`
	cd $dovecotdir
	abs_dovecotdir=`pwd`
	cd $old
	DISTCHECK_CONFIGURE_FLAGS="--with-dovecot=$abs_dovecotdir --without-dovecot-install-dirs"

	eval `grep -i '^dovecot_[[a-z_]]*=' "$dovecotdir"/dovecot-config`
	eval `grep '^LIBDOVECOT[[A-Z_]]*=' "$dovecotdir"/dovecot-config`

	if test "$use_install_dirs" = "no"; then
		# the main purpose of these is to fix make distcheck for plugins
		# other than that, they don't really make much sense
		dovecot_pkgincludedir='$(pkgincludedir)'
		dovecot_pkglibdir='$(pkglibdir)'
		dovecot_pkglibexecdir='$(libexecdir)/dovecot'
		dovecot_docdir='$(docdir)'
		dovecot_moduledir='$(moduledir)'
	fi

	AX_SUBST_L([DISTCHECK_CONFIGURE_FLAGS], [dovecotdir], [dovecot_moduledir], [dovecot_pkgincludedir], [dovecot_pkglibexecdir], [dovecot_pkglibdir], [dovecot_docdir])
	AX_SUBST_L([DOVECOT_CFLAGS], [DOVECOT_LIBS], [DOVECOT_SSL_LIBS], [DOVECOT_SQL_LIBS], [DOVECOT_COMPRESS_LIBS])
	AX_SUBST_L([LIBDOVECOT], [LIBDOVECOT_LOGIN], [LIBDOVECOT_SQL], [LIBDOVECOT_SSL], [LIBDOVECOT_COMPRESS], [LIBDOVECOT_LDA], [LIBDOVECOT_STORAGE], [LIBDOVECOT_DSYNC])
	AX_SUBST_L([LIBDOVECOT_DEPS], [LIBDOVECOT_LOGIN_DEPS], [LIBDOVECOT_SQL_DEPS], [LIBDOVECOT_SSL_DEPS], [LIBDOVECOT_COMPRESS_DEPS], [LIBDOVECOT_LDA_DEPS], [LIBDOVECOT_STORAGE_DEPS], [LIBDOVECOT_DSYNC_DEPS])
	AX_SUBST_L([LIBDOVECOT_INCLUDE], [LIBDOVECOT_LDA_INCLUDE], [LIBDOVECOT_DOVEADM_INCLUDE], [LIBDOVECOT_SERVICE_INCLUDE], [LIBDOVECOT_STORAGE_INCLUDE], [LIBDOVECOT_LOGIN_INCLUDE], [LIBDOVECOT_CONFIG_INCLUDE], [LIBDOVECOT_IMAP_INCLUDE], [LIBDOVECOT_DSYNC_INCLUDE])

	DC_PLUGIN_DEPS
])
