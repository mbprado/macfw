CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: fw410ctl

fw410ctl: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f fw410ctl

.PHONY: all clean
