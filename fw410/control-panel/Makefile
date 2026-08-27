CXX ?= c++
APP := build/macfw FW410 Control.app
CONTENTS := $(APP)/Contents
MACOS := $(CONTENTS)/MacOS
RESOURCES := $(CONTENTS)/Resources
BINARY := $(MACOS)/macfw-fw410-control
SOURCES := Sources/main.mm
ICON := icon.png
ICONSET := build/icon.iconset
ICNS := $(RESOURCES)/icon.icns

.PHONY: all clean run

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

$(BINARY): $(SOURCES) Info.plist $(ICNS)
	mkdir -p "$(MACOS)" "$(RESOURCES)"
	cp Info.plist "$(CONTENTS)/Info.plist"
	$(CXX) -std=c++17 -Wall -Wextra -Wpedantic -fobjc-arc \
		-framework AppKit -framework Foundation \
		-o "$(BINARY)" $(SOURCES)

run: all
	open "$(APP)"

clean:
	rm -rf build
