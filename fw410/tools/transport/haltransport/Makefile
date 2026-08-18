CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../hal/include

all: haltransport bridges

bridges:
	$(MAKE) -C ../halbridge44100
	$(MAKE) -C ../halbridge48000

haltransport: main.cpp ../../../hal/include/macfw_hal_shm.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f haltransport

.PHONY: all clean bridges
