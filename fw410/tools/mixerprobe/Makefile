CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

mixerprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -framework IOKit -framework CoreFoundation -o mixerprobe

clean:
	rm -f mixerprobe

.PHONY: clean
