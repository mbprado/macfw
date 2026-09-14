CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
TARGET := coreaudiodiag

.PHONY: all clean
all: $(TARGET)
$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $(TARGET)
clean:
	rm -f $(TARGET)
