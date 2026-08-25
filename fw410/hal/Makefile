BUNDLE := build/macfw-fw410.driver
CONTENTS := $(BUNDLE)/Contents
MACOS := $(CONTENTS)/MacOS

CXX ?= clang++
GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -DMACFW_GIT_SHA=\"$(GIT_SHA)\"
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation
SRC := src/FW410HALInstrumented.cpp src/build_identity.cpp

.PHONY: all clean install uninstall

all: $(MACOS)/macfw-fw410

$(MACOS)/macfw-fw410: $(SRC) src/FW410HALBridge.cpp include/macfw_hal_shm.h include/macfw_hal_capture_shm.h include/macfw_hal_transport_status.h ../version.h Info.plist
	mkdir -p $(MACOS)
	cp Info.plist $(CONTENTS)/Info.plist
	/usr/libexec/PlistBuddy -c "Delete :MacFWGitCommit" $(CONTENTS)/Info.plist >/dev/null 2>&1 || true
	/usr/libexec/PlistBuddy -c "Add :MacFWGitCommit string $(GIT_SHA)" $(CONTENTS)/Info.plist
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

install: $(MACOS)/macfw-fw410
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	chown -R root:wheel /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	killall coreaudiod

uninstall:
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw410.driver
	killall coreaudiod

clean:
	rm -rf build
