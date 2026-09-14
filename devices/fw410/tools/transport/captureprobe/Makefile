CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../hal/include

all: captureprobe

captureprobe: main.cpp ../../../hal/include/macfw_hal_capture_shm.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f captureprobe

.PHONY: all clean
