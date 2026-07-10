include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickeldissolve.so
override SOURCES  += src/config.c src/nickeldissolve.cc

# No Qt needed — this mod only hooks plain settings getters (bool/int returns), no QString.

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/doc:$(NDS_CONFIG_DIR)/doc res/default:$(NDS_CONFIG_DIR)/default res/uninstall:$(NDS_CONFIG_DIR)/uninstall

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickeldissolve.so
.PHONY: strip

ifeq ($(NDS_CONFIG_DIR),)
override NDS_CONFIG_DIR := /mnt/onboard/.adds/nickeldissolve
endif

override CPPFLAGS += -DNDS_CONFIG_DIR='"$(NDS_CONFIG_DIR)"' -DNDS_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NDS_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
