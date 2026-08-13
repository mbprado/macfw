CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

all: isoduplex

isoduplex: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $@

clean:
	rm -f isoduplex

.PHONY: all clean
