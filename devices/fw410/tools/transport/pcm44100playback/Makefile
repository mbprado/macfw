CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../../lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := pcm44100playback
LIB := ../../../lib/libmacfw.a

.PHONY: all clean script
all: $(TARGET) script

$(LIB):
	$(MAKE) -C ../../../lib libmacfw.a

$(TARGET): main.cpp $(LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(LIB) $(FRAMEWORKS) -o $(TARGET)

script:
	chmod +x run44100.sh

clean:
	rm -f $(TARGET)
