BUNDLE := build/macfw-fw410.driver
CONTENTS := $(BUNDLE)/Contents
MACOS := $(CONTENTS)/MacOS

CXX ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation

.PHONY: all clean install uninstall

all: $(MACOS)/macfw-fw410

$(MACOS)/macfw-fw410: src/FW410HAL.cpp Info.plist
	mkdir -p $(MACOS)
	cp Info.plist $(CONTENTS)/Info.plist
	$(CXX) $(CXXFLAGS) src/FW410HAL.cpp $(LDFLAGS) -o $@

install: all
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	chown -R root:wheel /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver

uninstall:
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver

clean:
	rm -rf build
