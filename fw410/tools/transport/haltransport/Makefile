CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../hal/include

all: haltransport bridges fwboot

bridges:
	$(MAKE) -C ../halbridge44100
	$(MAKE) -C ../halbridge48000

fwboot:
	$(MAKE) -C ../../device/fwboot

haltransport: main.cpp ../../../hal/include/macfw_hal_shm.h fwboot
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f haltransport

.PHONY: all clean bridges fwboot
