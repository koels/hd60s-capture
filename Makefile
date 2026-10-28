CC = clang
# One version, in VERSION. The dotted string fills in each bundle's
# CFBundleVersion; the same numbers packed as major*10000 + minor*100 + patch
# become HD_SHARED_BUILD, which is what a client compares against a running
# service. Editing either by hand is what let a stale service keep the card.
VERSION := $(shell cat VERSION)
BUILD := $(shell awk -F. '{printf "%d",$$1*10000+$$2*100+$$3}' VERSION)
VERSIONFLAG = -DHD_SHARED_BUILD=$(BUILD)
CFLAGS = -std=c11 -O2 -Wall -Wextra $(VERSIONFLAG)
TESTFLAGS = -std=c11 -O1 -g -Wall -Wextra $(VERSIONFLAG) -fsanitize=address,undefined
FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework CoreServices
# Keeps the bundles' version in step with VERSION without a second edit.
define fill_version
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's|@FEED@|$(FEED)|g' -e 's|@EDKEY@|$(EDKEY)|g' $(1) > $(2)
endef
OBS_APP ?= /Applications/OBS.app
OBS_SRC ?= .deps/obs
SIMDE_SRC ?= .deps/simde
SPARKLE_SRC ?= .deps/sparkle
# Filled into the app's Info.plist. Without a feed the updater is never started
# and the menu item says so, so the app builds and runs the same either way.
FEED := $(shell cat packaging/appcast-url 2>/dev/null)
EDKEY := $(shell cat packaging/appcast-key.pub 2>/dev/null)
PLUGIN = build/hd60s-native.plugin
INSTALL_DIR = $(HOME)/Library/Application Support/obs-studio/plugins/hd60s-native.plugin

.PHONY: all probe test plugin install clean
all: build/hd60s-probe build/hd60s-capture
build/hd60s-probe: src/probe.c
	mkdir -p build
	$(CC) $(CFLAGS) $< $(FRAMEWORKS) -o $@
build/hd60s-capture: src/capture.c src/device.c src/capture.h src/main.c
	mkdir -p build
	$(CC) $(CFLAGS) src/capture.c src/device.c src/main.c $(FRAMEWORKS) -o $@
probe: build/hd60s-probe
	./build/hd60s-probe
build/test-parser: tests/parser.c src/capture.c src/device.c src/capture.h
	mkdir -p build
	$(CC) $(TESTFLAGS) tests/parser.c src/device.c $(FRAMEWORKS) -o $@
build/test-processing: tests/processing.c src/processing.h
	mkdir -p build
	$(CC) $(TESTFLAGS) tests/processing.c -o $@
build/test-scaler: tests/scaler.c
	$(CC) $(CFLAGS) tests/scaler.c -I$(OBS_SRC)/libobs -I$(SIMDE_SRC) -Ibuild/include -F"$(OBS_APP)/Contents/Frameworks" -framework libobs -Wl,-rpath,"$(OBS_APP)/Contents/Frameworks" -o $@
test-video: build/test-scaler
	./build/test-scaler
build/test-status-frame: tests/status-frame.c src/status-frame.c src/status-frame.h src/capture.h
	$(CC) $(TESTFLAGS) tests/status-frame.c src/status-frame.c -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework ImageIO -o $@
build/test-service-identity: tests/service-identity.c src/shared-internal.h src/capture.h
	$(CC) $(TESTFLAGS) tests/service-identity.c -o $@
test: build/test-parser build/test-processing build/test-status-frame build/test-service-identity
	./build/test-parser
	./build/test-processing
	./build/test-status-frame
	./build/test-service-identity
plugin: service $(PLUGIN)/Contents/MacOS/hd60s-native
	cp build/hd60s-service $(PLUGIN)/Contents/MacOS/
	codesign --force --sign - $(PLUGIN)
$(PLUGIN)/Contents/MacOS/hd60s-native: src/plugin.c src/shared-client.c src/shared.h src/shared-internal.h src/status-frame.c src/status-frame.h src/capture.h src/processing.h packaging/Info.plist packaging/obsconfig.h VERSION
	mkdir -p $(PLUGIN)/Contents/MacOS build/include
	cp packaging/obsconfig.h build/include/
	$(CC) $(CFLAGS) -fPIC -bundle src/plugin.c src/shared-client.c src/status-frame.c -I$(OBS_SRC)/libobs -I$(SIMDE_SRC) -Ibuild/include -F"$(OBS_APP)/Contents/Frameworks" -framework libobs -framework CoreGraphics -framework CoreText $(FRAMEWORKS) -Wl,-rpath,"$(OBS_APP)/Contents/Frameworks" -o $@
	$(call fill_version,packaging/Info.plist,$(PLUGIN)/Contents/Info.plist)
	codesign --force --sign - $(PLUGIN)
install: plugin
	mkdir -p "$(INSTALL_DIR)/Contents/MacOS"
	cp $(PLUGIN)/Contents/MacOS/hd60s-native "$(INSTALL_DIR)/Contents/MacOS/"
	cp $(PLUGIN)/Contents/Info.plist "$(INSTALL_DIR)/Contents/"
	cp build/hd60s-service "$(INSTALL_DIR)/Contents/MacOS/"
	codesign --force --sign - "$(INSTALL_DIR)"
clean:
	rm -f build/hd60s-probe build/hd60s-capture build/test-parser $(PLUGIN)/Contents/MacOS/hd60s-native

APP = build/HD60 S Capture.app
.PHONY: app
app: service build/AppIcon.icns app/PluginInstaller.m app/PluginInstaller.h src/status-frame.c src/status-frame.h src/shared-client.c src/shared.h src/shared-internal.h src/device.c src/capture.h src/processing.h app/main.m packaging/AppInfo.plist VERSION $(SPARKLE_SRC)/Sparkle.framework
	mkdir -p "$(APP)/Contents/MacOS"
	$(CC) $(CFLAGS) -Isrc -c src/device.c -o build/app-device.o
	$(CC) $(CFLAGS) -Isrc -c src/shared-client.c -o build/app-shared.o
	$(CC) $(CFLAGS) -Isrc -c src/status-frame.c -o build/app-status.o
	$(CC) -O2 -Wall -Wextra $(VERSIONFLAG) -fobjc-arc -Isrc app/main.m app/PluginInstaller.m build/app-device.o build/app-shared.o build/app-status.o $(FRAMEWORKS) -framework Cocoa -framework CoreText -framework QuartzCore -framework CoreImage -framework CoreVideo -framework AVFoundation -framework CoreMedia -framework AudioToolbox -F$(SPARKLE_SRC) -framework Sparkle -rpath @executable_path/../Frameworks -o "$(APP)/Contents/MacOS/hd60s-app"
	mkdir -p "$(APP)/Contents/Resources"
	cp build/AppIcon.icns "$(APP)/Contents/Resources/"
	@if [ -d "$(PLUGIN)" ]; then rm -rf "$(APP)/Contents/Resources/hd60s-native.plugin"; ditto "$(PLUGIN)" "$(APP)/Contents/Resources/hd60s-native.plugin"; echo "embedded OBS plugin"; else echo "no OBS plugin built; app will not offer to install one"; fi
	$(call fill_version,packaging/AppInfo.plist,"$(APP)/Contents/Info.plist")
	cp build/hd60s-service "$(APP)/Contents/MacOS/"
	mkdir -p "$(APP)/Contents/Frameworks"
	rm -rf "$(APP)/Contents/Frameworks/Sparkle.framework"
	ditto $(SPARKLE_SRC)/Sparkle.framework "$(APP)/Contents/Frameworks/Sparkle.framework"
	@# Signed inside out: a nested bundle signed after its container invalidates
	@# the container's signature, and Sparkle's installer is four levels deep.
	codesign --force --sign - "$(APP)/Contents/Frameworks/Sparkle.framework/Versions/B/XPCServices/Downloader.xpc"
	codesign --force --sign - "$(APP)/Contents/Frameworks/Sparkle.framework/Versions/B/XPCServices/Installer.xpc"
	codesign --force --sign - "$(APP)/Contents/Frameworks/Sparkle.framework/Versions/B/Autoupdate"
	codesign --force --sign - "$(APP)/Contents/Frameworks/Sparkle.framework/Versions/B/Updater.app"
	codesign --force --sign - "$(APP)/Contents/Frameworks/Sparkle.framework"
	codesign --force --sign - "$(APP)"

.PHONY: bundle
bundle: plugin app

.PHONY: icon
ICONSET = build/HD60S.iconset
icon: build/AppIcon.icns
build/hd60s-icon: tools/make-icon.c
	mkdir -p build
	$(CC) $(CFLAGS) tools/make-icon.c -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o $@
build/AppIcon.icns: build/hd60s-icon
	rm -rf "$(ICONSET)"
	mkdir -p "$(ICONSET)"
	./build/hd60s-icon "$(ICONSET)"
	iconutil -c icns "$(ICONSET)" -o $@

.PHONY: service
service: build/hd60s-service
build/hd60s-service: src/service.c src/shared-internal.h src/capture.c src/device.c src/capture.h VERSION
	mkdir -p build
	$(CC) $(CFLAGS) src/service.c src/capture.c src/device.c $(FRAMEWORKS) -o $@

.PHONY: test-shared
build/test-shared-probe: tests/shared-probe.c src/shared-client.c src/shared.h src/shared-internal.h
	$(CC) $(TESTFLAGS) tests/shared-probe.c src/shared-client.c $(FRAMEWORKS) -o $@
test-shared: service build/test-shared-probe
	python3 tools/test-shared.py
