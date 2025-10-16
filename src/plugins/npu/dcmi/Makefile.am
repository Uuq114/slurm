# Makefile for npu/dcmi plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir) -I$(top_srcdir)/src/interfaces $(DCMI_CPPFLAGS)

DCMI_SOURCES = npu_dcmi.c

pkglib_LTLIBRARIES = npu_dcmi.la

npu_dcmi_la_SOURCES = $(DCMI_SOURCES)
# npu_dcmi_la_LDFLAGS = $(PLUGIN_FLAGS) -export-symbols-regex '^npu_p_' \
#                       -L/usr/local/Ascend/driver/lib64/driver \
#                       -Wl,-rpath=/usr/local/Ascend/driver/lib64/driver
npu_dcmi_la_LDFLAGS = $(PLUGIN_FLAGS) \
                      -L/usr/local/Ascend/driver/lib64/driver \
                      -Wl,-rpath=/usr/local/Ascend/driver/lib64/driver
npu_dcmi_la_LIBADD = -ldcmi -lascend_hal -ldrvdsmi_host