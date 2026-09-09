CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: fw1814state

fw1814state: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f fw1814state

.PHONY: all clean
