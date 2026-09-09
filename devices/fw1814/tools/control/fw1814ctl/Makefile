CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: fw1814ctl

fw1814ctl: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f fw1814ctl

.PHONY: all clean
