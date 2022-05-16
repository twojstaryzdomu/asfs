#
# Makefile for the linux asfs filesystem routines.
#

obj-$(CONFIG_ASFS_FS) += asfs.o

asfs-y += dir.o extents.o file.o inode.o namei.o nodes.o objects.o super.o symlink.o
asfs-$(CONFIG_ASFS_RW) += adminspace.o bitfuncs.o 

KDIR    := /lib/modules/$(shell uname -r)/build
#KDIR	:= /usr/src/linux-2.6.27
PWD     := $(shell pwd)

EXTRA_CFLAGS += -DCONFIG_ASFS_DEFAULT_CODEPAGE="\"$(CONFIG_ASFS_DEFAULT_CODEPAGE)\""
ifeq ($(CONFIG_ASFS_RW), y)
	EXTRA_CFLAGS += -DCONFIG_ASFS_RW=$(CONFIG_ASFS_RW)
endif

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
