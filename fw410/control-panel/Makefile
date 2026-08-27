CXX ?= c++
APP := build/macfw FW410 Control.app
CONTENTS := $(APP)/Contents
MACOS := $(CONTENTS)/MacOS
BINARY := $(MACOS)/macfw-fw410-control
SOURCES := Sources/main.mm

.PHONY: all clean run

all: $(BINARY)

$(BINARY): $(SOURCES) Info.plist
	mkdir -p "$(MACOS)"
	cp Info.plist "$(CONTENTS)/Info.plist"
	$(CXX) -std=c++17 -Wall -Wextra -Wpedantic -fobjc-arc \
		-framework AppKit -framework Foundation \
		-o "$(BINARY)" $(SOURCES)

run: all
	open "$(APP)"

clean:
	rm -rf build
