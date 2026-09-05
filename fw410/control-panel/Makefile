CXX ?= c++
GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CPPFLAGS += -I.. -DMACFW_GIT_SHA=\"$(GIT_SHA)\"
APP := build/macfw FW410 Control.app
INSTALL_APP := /Applications/macfw FW410 Control.app
CONTENTS := $(APP)/Contents
MACOS := $(CONTENTS)/MacOS
RESOURCES := $(CONTENTS)/Resources
BINARY := $(MACOS)/macfw-fw410-control
SOURCES := Sources/main.mm Sources/outputs_extension.mm Sources/mixer_extension.mm Sources/slider_async_extension.mm Sources/headphone_mixer_async_extension.mm Sources/meter_extension.mm Sources/device_extension.mm Sources/diagnostics_extension.mm
ICON := icon.png
ICONSET := build/icon.iconset
ICNS := $(RESOURCES)/icon.icns

.PHONY: all clean run install uninstall

all: $(BINARY)

$(ICNS): $(ICON)
	rm -rf "$(ICONSET)"
	mkdir -p "$(ICONSET)" "$(RESOURCES)"
	sips -z 16 16 "$(ICON)" --out "$(ICONSET)/icon_16x16.png" >/dev/null
	sips -z 32 32 "$(ICON)" --out "$(ICONSET)/icon_16x16@2x.png" >/dev/null
	sips -z 32 32 "$(ICON)" --out "$(ICONSET)/icon_32x32.png" >/dev/null
	sips -z 64 64 "$(ICON)" --out "$(ICONSET)/icon_32x32@2x.png" >/dev/null
	sips -z 128 128 "$(ICON)" --out "$(ICONSET)/icon_128x128.png" >/dev/null
	sips -z 256 256 "$(ICON)" --out "$(ICONSET)/icon_128x128@2x.png" >/dev/null
	sips -z 256 256 "$(ICON)" --out "$(ICONSET)/icon_256x256.png" >/dev/null
	sips -z 512 512 "$(ICON)" --out "$(ICONSET)/icon_256x256@2x.png" >/dev/null
	sips -z 512 512 "$(ICON)" --out "$(ICONSET)/icon_512x512.png" >/dev/null
	iconutil -c icns "$(ICONSET)" -o "$(ICNS)"
	cp "$(ICON)" "$(RESOURCES)/icon.png"

$(BINARY): $(SOURCES) Sources/stereo_link.h Sources/slider_commit.h Info.plist $(ICNS) ../version.h
	mkdir -p "$(MACOS)" "$(RESOURCES)"
	cp Info.plist "$(CONTENTS)/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(GIT_SHA)" "$(CONTENTS)/Info.plist"
	/usr/libexec/PlistBuddy -c "Delete :MacFWGitCommit" "$(CONTENTS)/Info.plist" >/dev/null 2>&1 || true
	/usr/libexec/PlistBuddy -c "Add :MacFWGitCommit string $(GIT_SHA)" "$(CONTENTS)/Info.plist"
	$(CXX) $(CPPFLAGS) -std=c++17 -Wall -Wextra -Wpedantic -fobjc-arc \
		-framework AppKit -framework Foundation -framework CoreAudio \
		-o "$(BINARY)" $(SOURCES)

run: all
	open "$(APP)"

install:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: control-panel install must be run as root" >&2; \
		exit 1; \
	fi
	@if [ ! -x "$(BINARY)" ]; then \
		echo "error: control panel is not built; run 'make gui' as a normal user first" >&2; \
		exit 1; \
	fi
	rm -rf "$(INSTALL_APP)"
	cp -R "$(APP)" "$(INSTALL_APP)"
	chown -R root:wheel "$(INSTALL_APP)"

uninstall:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: control-panel uninstall must be run as root" >&2; \
		exit 1; \
	fi
	rm -rf "$(INSTALL_APP)"

clean:
	rm -rf build
