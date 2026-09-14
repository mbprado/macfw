CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../../lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := cmpprobe
LIBMACFW := ../../../lib/libmacfw.a

all: $(TARGET)

$(LIBMACFW):
	$(MAKE) -C ../../../lib libmacfw.a

$(TARGET): main.cpp $(LIBMACFW)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
