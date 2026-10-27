# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Hisilicon processors family
#

CFLAGS_platmcpm.o	:= -march=armv7-a

obj-y	+= hisilicon.o
obj-$(CONFIG_MCPM)		+= platmcpm.o
obj-$(CONFIG_SMP)		+= platsmp.o hotplug.o
obj-$(CONFIG_HISTB_CV200_PMUDUMP) += cv200-pmudump.o
