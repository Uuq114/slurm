# Makefile for npu/dcmi plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir) $(DCMI_CPPFLAGS)

DCMI_SOURCES = npu_dcmi.c

pkglib_LTLIBRARIES = npu_dcmi.la

npu_dcmi_la_SOURCES = $(DCMI_SOURCES)
npu_dcmi_la_LDFLAGS = $(PLUGIN_FLAGS)
# npu_dcmi_la_LIBADD = $(DCMI_LIBS)
