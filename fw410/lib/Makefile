CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude

all: smoke

smoke: src/smoke.cpp include/macfw/channel_map.h include/macfw/am824.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/smoke.cpp -o $@

check: smoke
	./smoke

clean:
	rm -f smoke

.PHONY: all check clean
