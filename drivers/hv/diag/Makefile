# SPDX-License-Identifier: GPL-2.0
# Makefile for the mshv diagnostics driver (mshv_diag).

obj-$(CONFIG_MSHV_DIAG)	+= mshv_diag.o

CFLAGS_mshv_diag_main.o	= -DHV_HYPERV_DEFS
CFLAGS_mshv_diaglog.o	= -DHV_HYPERV_DEFS

mshv_diag-y     := mshv_diag_main.o mshv_diaglog.o