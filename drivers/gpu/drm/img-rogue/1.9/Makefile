img_basedir := drivers/gpu/drm/img-rogue/1.9
include $(img_basedir)/config_kernel.mk

obj-$(CONFIG_DRM_POWERVR_ROGUE_1_9) += pvrsrvkm_1_9.o

ccflags-y += \
 -include config_kernel.h \
 -Iinclude/drm \
 -I$(img_basedir) \
 -I$(img_basedir)/km \
 -I$(img_basedir)/system \
 -D__linux__

include $(img_basedir)/pvrsrvkm.mk
include $(img_basedir)/mt8173/Makefile
