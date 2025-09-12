# Makefile for gres/mygres plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = gres_mygres.la

# Gres mygres plugin.
gres_mygres_la_SOURCES = gres_mygres.c
gres_mygres_la_LDFLAGS = $(PLUGIN_FLAGS)
gres_mygres_la_LIBADD = ../common/libgres_common.la

force:
$(gres_mygres_la_LIBADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`


# AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

# pkglib_LTLIBRARIES = gres_mygres.la

# gres_mygres_la_SOURCES = mygres.c
# gres_mygres_la_LDFLAGS = $(PLUGIN_FLAGS)
# gres_mygres_la_LIBADD = $(LIB_SLURM)