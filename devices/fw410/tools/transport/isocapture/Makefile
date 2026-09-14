CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../../lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation
LIB := ../../../lib/libmacfw.a

.PHONY: all clean

all: isocapture

$(LIB):
	$(MAKE) -C ../../../lib libmacfw.a

isocapture: main.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LIB) $(FRAMEWORKS) -o $@

clean:
	rm -f isocapture
