CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS += -framework CoreAudio

all: shmprobe

shmprobe: main.cpp ../../../hal/include/macfw_hal_shm.h
	$(CXX) $(CXXFLAGS) main.cpp $(LDFLAGS) -o $@

clean:
	rm -f shmprobe

.PHONY: all clean
