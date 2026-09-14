CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS ?= -framework IOKit -framework CoreFoundation

TARGET := fwboot
SOURCES := main.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
