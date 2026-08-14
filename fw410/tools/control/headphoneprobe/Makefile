CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS = -framework IOKit -framework CoreFoundation

all: headphoneprobe

headphoneprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $@

clean:
	rm -f headphoneprobe

.PHONY: all clean
