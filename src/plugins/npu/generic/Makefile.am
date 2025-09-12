# Makefile for npu/generic plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = npu_generic.la

# NPU GENERIC plugin.
npu_generic_la_SOURCES = npu_generic.c
npu_generic_la_LDFLAGS = $(PLUGIN_FLAGS)

force:
$(npu_generic_la_LIBADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`
