BUNDLE := build/macfw-fw1814.driver
CONTENTS := $(BUNDLE)/Contents
MACOS := $(CONTENTS)/MacOS

CXX ?= clang++
GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -DMACFW_GIT_SHA=\"$(GIT_SHA)\"
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation
SRC := src/FW1814HAL.cpp

.PHONY: all clean install uninstall

all: $(MACOS)/macfw-fw1814

$(MACOS)/macfw-fw1814: $(SRC) include/macfw_fw1814_hal_shm.h include/macfw_fw1814_capture_shm.h Info.plist
	mkdir -p $(MACOS)
	cp Info.plist $(CONTENTS)/Info.plist
	/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(GIT_SHA)" $(CONTENTS)/Info.plist
	/usr/libexec/PlistBuddy -c "Delete :MacFWGitCommit" $(CONTENTS)/Info.plist >/dev/null 2>&1 || true
	/usr/libexec/PlistBuddy -c "Add :MacFWGitCommit string $(GIT_SHA)" $(CONTENTS)/Info.plist
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

install:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: HAL install must be run as root" >&2; \
		exit 1; \
	fi
	@if [ ! -x "$(MACOS)/macfw-fw1814" ]; then \
		echo "error: HAL bundle is not built; run 'make' as a normal user first" >&2; \
		exit 1; \
	fi
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver
	cp -R $(BUNDLE) /Library/Audio/Plug-Ins/HAL/
	chown -R root:wheel /Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver
	killall coreaudiod

uninstall:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: HAL uninstall must be run as root" >&2; \
		exit 1; \
	fi
	rm -rf /Library/Audio/Plug-Ins/HAL/macfw-fw1814.driver
	killall coreaudiod

clean:
	rm -rf build
