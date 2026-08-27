SWIFTC ?= xcrun swiftc
TARGET ?= x86_64-apple-macosx12.0
APP := build/macfw FW410 Control.app
CONTENTS := $(APP)/Contents
MACOS := $(CONTENTS)/MacOS
BINARY := $(MACOS)/macfw-fw410-control
SOURCES := Sources/MacFW410Control.swift
MODULE_CACHE := build/swift-module-cache

.PHONY: all clean run

all: $(BINARY)

$(BINARY): $(SOURCES) Info.plist
	mkdir -p "$(MACOS)" "$(MODULE_CACHE)"
	cp Info.plist "$(CONTENTS)/Info.plist"
	$(SWIFTC) -target $(TARGET) -Onone \
		-module-cache-path "$(MODULE_CACHE)" \
		-framework SwiftUI -framework AppKit \
		-o "$(BINARY)" $(SOURCES)

run: all
	open "$(APP)"

clean:
	rm -rf build
