CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS ?= -framework IOKit -framework CoreFoundation

all: fcpprobe

fcpprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(LDFLAGS) -o $@

clean:
	rm -f fcpprobe

.PHONY: all clean
