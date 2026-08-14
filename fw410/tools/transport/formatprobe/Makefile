CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := formatprobe
SOURCES := main.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(FRAMEWORKS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
