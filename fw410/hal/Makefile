BUNDLE := build/macfw-fw410.driver
CONTENTS := $(BUNDLE)/Contents
MACOS := $(CONTENTS)/MacOS

CXX ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation
SRC := src/FW410HALBridge.cpp

.PHONY: all clean install uninstall shmprobe

all: $(MACOS)/macfw-fw410 shmprobe

$(MACOS)/macfw-fw410: $(SRC) include/macfw_hal_shm.h Info.plist
	mkdir -p $(MACOS)
	cp Info.plist $(CONTENTS)/Info.plist
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

shmprobe: tools/shmprobe.cpp include/macfw_hal_shm.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) tools/shmprobe.cpp -o build/shmprobe

install: $(MACOS)/macfw-fw410
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	chown -R root:wheel /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	killall coreaudiod
uninstall:
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver

clean:
	rm -rf build
