CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := cmpprobe

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
