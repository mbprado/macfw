CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

.PHONY: all clean

all: isocapture

isocapture: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $@

clean:
	rm -f isocapture
