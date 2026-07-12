include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickeldissolve.so
override SOURCES  += src/config.c src/nickeldissolve.cc src/gesture.cc src/settingsui.cc
override MOCS     += src/ndsbridge.h

# QtCore/QtGui: the app-wide gesture event filter and reader-state tracking (plain QObject).
# QtWidgets + the moc'd NdsBridge: the "Page turn animations" toggle inserted into the Reading
# settings page (a native SettingItemWithToggleSwitch row).
override PKGCONF  += Qt5Widgets

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/doc:$(NDS_CONFIG_DIR)/doc res/uninstall:$(NDS_CONFIG_DIR)/uninstall

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickeldissolve.so
.PHONY: strip

ifeq ($(NDS_CONFIG_DIR),)
override NDS_CONFIG_DIR := /mnt/onboard/.adds/nickel-dissolve
endif

override CPPFLAGS += -DNDS_CONFIG_DIR='"$(NDS_CONFIG_DIR)"' -DNDS_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NDS_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
