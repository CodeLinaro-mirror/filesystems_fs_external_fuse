#
# Makefile for the FUSE filesystem.
#

obj-$(CONFIG_FUSE2_CORE) += fuse2_core.o
obj-$(CONFIG_FUSE2_FS) += fuse2.o

fuse2-y := fudev.o
fuse2_core-y := request.o dir.o file.o inode.o xattr.o acl.o readdir.o map.o
