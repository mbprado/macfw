BUNDLE := build/macfw-fw410.driver
CONTENTS := $(BUNDLE)/Contents
MACOS := $(CONTENTS)/MacOS

CXX ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation
SRC := src/FW410HALInstrumented.cpp

.PHONY: all clean install uninstall

all: $(MACOS)/macfw-fw410

$(MACOS)/macfw-fw410: $(SRC) src/FW410HALBridge.cpp include/macfw_hal_shm.h Info.plist
	mkdir -p $(MACOS)
	cp Info.plist $(CONTENTS)/Info.plist
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

install: $(MACOS)/macfw-fw410
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	chown -R root:wheel /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	killall coreaudiod

uninstall:
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver

clean:
	rm -rf build
