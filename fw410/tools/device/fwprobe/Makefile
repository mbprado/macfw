CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

TARGET := fwprobe
SRC := main.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) $(FRAMEWORKS) -o $@

clean:
	rm -f $(TARGET)
