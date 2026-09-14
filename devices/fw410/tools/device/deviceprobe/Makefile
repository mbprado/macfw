CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CPPFLAGS += -DMACFW_GIT_SHA=\"$(GIT_SHA)\"
LDFLAGS := -framework IOKit -framework CoreFoundation

all: deviceprobe

deviceprobe: main.cpp device_registry.h ../../../version.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LDFLAGS) -o $@

clean:
	rm -f deviceprobe

.PHONY: all clean
