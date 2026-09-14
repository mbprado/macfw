CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../../devices/fw410/lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := amdtpcaptureprobe
LIBMACFW := ../../../devices/fw410/lib/libmacfw.a

.PHONY: all clean

all: $(TARGET)

$(LIBMACFW):
	$(MAKE) -C ../../../devices/fw410/lib libmacfw.a

$(TARGET): main.cpp $(LIBMACFW)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $(TARGET)

clean:
	rm -f $(TARGET)
