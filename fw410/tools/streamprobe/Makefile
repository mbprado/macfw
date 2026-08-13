CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS ?= -framework IOKit -framework CoreFoundation

all: streamprobe

streamprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(LDFLAGS) -o $@

clean:
	rm -f streamprobe

.PHONY: all clean
