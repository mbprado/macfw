CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../../lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioUnit -framework AudioToolbox
TARGET := coreaudiobridge
LIB := ../../../lib/libmacfw.a

.PHONY: all clean

all: $(TARGET)

$(LIB):
	$(MAKE) -C ../../../lib libmacfw.a

$(TARGET): main.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LIB) $(FRAMEWORKS) -o $(TARGET)

clean:
	rm -f $(TARGET)
