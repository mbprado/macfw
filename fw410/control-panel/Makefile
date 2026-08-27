SWIFTC ?= xcrun swiftc
TARGET ?= x86_64-apple-macosx12.0
APP := build/macfw FW410 Control.app
CONTENTS := $(APP)/Contents
MACOS := $(CONTENTS)/MacOS
BINARY := $(MACOS)/macfw-fw410-control
SOURCES := Sources/main.swift

.PHONY: all clean run

all: $(BINARY)

$(BINARY): $(SOURCES) Info.plist
	mkdir -p "$(MACOS)"
	cp Info.plist "$(CONTENTS)/Info.plist"
	$(SWIFTC) -target $(TARGET) -O \
		-framework SwiftUI -framework AppKit \
		-o "$(BINARY)" $(SOURCES)

run: all
	open "$(APP)"

clean:
	rm -rf build
