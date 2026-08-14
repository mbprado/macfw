CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS = -framework IOKit -framework CoreFoundation

all: plugprobe

plugprobe: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp $(FRAMEWORKS) -o $@

clean:
	rm -f plugprobe
