#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Version that can use drv files instead of ppd files.
$cupsversionfordrv=1.6

AC_PREREQ([2.6])
AC_INIT([c2esp], [27], quandry@ntlworld.com)
AM_INIT_AUTOMAKE
AC_CONFIG_HEADERS([config.h])
#AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC
AC_CHECK_PROG(HAVE_PPDC, ppdc, 1)
AC_CHECK_PROG(HAVE_CUPSCONFIG, cups-config, 1)

# Checks for libraries.
AC_CHECK_LIB(cups, ppdOpenFile)

#-lcupsdriver for cups < ver 1.6
AC_SEARCH_LIBS(cupsDitherNew, cupsdriver cupsfilters)
if test "$ac_cv_search_cupsDitherNew" = "-lcupsdriver"; then :
	HAVE_LIBCUPSDRIVER="1"
fi
if test "$ac_cv_search_cupsDitherNew" = "-lcupsfilters"; then :
	HAVE_LIBCUPSFILTERS="1"
fi

AC_SEARCH_LIBS(cupsRasterOpen, cupsimage cupsfilters cups)
if test "$ac_cv_search_cupsRasterOpen" != "no"; then :
	HAVE_LIBCUPSIMAGE="1"
fi

AC_SEARCH_LIBS(deflate, z)
if test "$ac_cv_search_deflate" != "no"; then :
	HAVE_LIBZ="1"
fi
# In order to work with Debian or Fedora
AC_SEARCH_LIBS(jbg85_enc_init, jbig85 jbig)
JBIG85LIB=$ac_cv_search_jbg85_enc_init

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h unistd.h cups/raster.h cups/sidechannel.h])
AC_CHECK_HEADERS([cups/image.h cups/driver.h])
AC_CHECK_HEADERS([cupsfilters/image.h cupsfilters/driver.h])
AC_CHECK_HEADERS([zlib.h])
AC_CHECK_HEADERS([jbig85.h jbig.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_CHECK_FUNCS([memset strerror strncasecmp strstr])

# Identify cups version
if test "$HAVE_CUPSCONFIG" = "1"; then :
	cupsversion=$(cups-config --version)
	AC_MSG_NOTICE([cupsversion is $cupsversion])
	AC_SUBST(cupsversion)
fi

# Set cups filter installation directory
if test "$HAVE_CUPSCONFIG" = "1"; then :
	defaultfilterdir=$(cups-config --serverbin)/filter
	echo "default filter dir $defaultfilterdir is from cups-config"
else
	defaultfilterdir="/usr/lib/cups/filter"
	echo "no cups-config, default filter dir will be $defaultfilterdir"
fi

AC_ARG_WITH(cupsfilterdir, AC_HELP_STRING([--with-cupsfilterdir=DIR], [set cups filter install directory [default is supplied by cups-config]]),
   cupsfilterdir=$withval, cupsfilterdir=$defaultfilterdir)
AC_MSG_NOTICE([cupsfilterdir is $cupsfilterdir])
AC_SUBST(cupsfilterdir)

# Set cups data installation directory
if test "$HAVE_CUPSCONFIG" = "1"; then :
	defaultdatadir=$(cups-config --datadir)
	echo "default data dir $defaultdatadir is from cups-config"
else
	defaultdatadir="/usr/share/cups"
	echo "no cups-config, default data dir will be $defaultdatadir"
fi

AC_ARG_WITH(cupsdatadir, AC_HELP_STRING([--with-cupsdatadir=DIR], [set cups data file install directory [default is supplied by cups-config]]),
   cupsdatadir=$withval, cupsdatadir=$defaultdatadir)
AC_MSG_NOTICE([cupsdatadir is $cupsdatadir])
AC_SUBST(cupsdatadir)

AC_MSG_NOTICE([HAVE_PPDC = $HAVE_PPDC])
ppdlist=""
if test "$HAVE_PPDC" = "1"; then :
	AC_MSG_NOTICE([making ppd files with ppdc])
	for drvfile in src/*.drv ; do
		ppdc $drvfile;
	done
# check the actual version of cups where drv files can be used instead of ppds
	if test "$cupsversion" \< "$cupsversionfordrv"; then
		cd ppd
		for ppdfile in *.ppd; do \
			ppdlist="$ppdlist $ppdfile"
		done;
		cd ..
		echo "ppds to be installed = $ppdlist"
	else
		echo "no ppds will be installed"
	fi
else
	if test "$cupsversion" \< "$cupsversionfordrv"; then
		AC_MSG_WARN([ppdc was not found. So ppd files were not created. ppd files are only required for old versions of cups that cannot use the drv files.])
	else
		AC_MSG_NOTICE([ppdc was not found but it's not needed for cups $cupsversionfordrv or later.])
	fi
fi
AC_SUBST(ppdlist)


AC_MSG_NOTICE([*])
AC_MSG_NOTICE([*** Any missing dependencies are listed below ***])
if test "$HAVE_CUPSCONFIG" != "1"; then :
	AC_MSG_WARN([cups-config was not found. Try installing a package like libcups2-dev and run configure again.])
fi

if test "$cupsversion" \< "1.6"; then
	AC_MSG_NOTICE([cups version is earlier than 1.6 - OK])
	if test "$HAVE_LIBCUPSDRIVER" != "1"; then :
		AC_MSG_WARN([libcupsdriver was not found. Try installing a package like libcupsdriver1-dev and run configure again.])
	fi
else
	AC_MSG_NOTICE([cups version is 1.6 or later - OK])
	if test "$HAVE_LIBCUPSFILTERS" != "1"; then :
		AC_MSG_WARN([HAVE_LIBCUPSFILTERS = $HAVE_LIBCUPSFILTERS libcupsfilters was not found. Try installing a package like libcupsfilters-dev and run configure again.])
	fi
fi

if test "$HAVE_LIBCUPSIMAGE" != "1"; then :
	AC_MSG_WARN([libcupsimage was not found. Try installing a package like libcupsimage2-dev and run configure again.])
fi
#AC_MSG_NOTICE([JBIG85LIB=$JBIG85LIB])
if test "$JBIG85LIB" != "-ljbig" && test "$JBIG85LIB" != "-ljbig85"; then :
		AC_MSG_WARN([jbig85 library was not found. Try installing a package like jbig-kit libjbig85 or libjbig and run configure again. If your distro is pre 2012 you may need to install jbigkit85])
fi
AC_MSG_NOTICE([*])
AC_MSG_NOTICE([LIBS=$LIBS])
AC_OUTPUT(Makefile src/Makefile doc/Makefile scripts/Makefile ppd/Makefile)
