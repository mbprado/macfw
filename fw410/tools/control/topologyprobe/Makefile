CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: topologyprobe

topologyprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -framework IOKit -framework CoreFoundation -o $@

clean:
	rm -f topologyprobe

.PHONY: all clean
