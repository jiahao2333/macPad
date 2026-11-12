ARCHS = arm64e
TARGET = iphone:clang:latest:14.0
include ../config/production.mk
include $(THEOS)/makefiles/common.mk

# Generic first image for launchd-managed overlay XPC bundles (field notes
# §59). Same freestanding source as the ViewBridge/WebKit proxies; the
# overlay mode derives the macOS target from <bundle>/MacWSTarget.
TOOL_NAME = MacWSXPCProxy
MacWSXPCProxy_FILES = ../ViewBridgeChrootProxy/main.c
MacWSXPCProxy_CFLAGS = -DMACWS_WEBKIT_PROXY_EXPERIMENT=1 -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
MacWSXPCProxy_LDFLAGS = -nostdlib -lSystem -Wl,-e,_main
MacWSXPCProxy_CODESIGN_FLAGS = -S../entitlements.plist
MacWSXPCProxy_INSTALL_PATH = /usr/macOS/libexec

include $(THEOS_MAKE_PATH)/tool.mk
