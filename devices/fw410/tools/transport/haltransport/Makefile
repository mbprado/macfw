CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
GIT_SHA := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
CPPFLAGS += -I../../../hal/include -DMACFW_GIT_SHA=\"$(GIT_SHA)\"

all: haltransport bridges fwboot

bridges:
	$(MAKE) -C ../halbridge44100
	$(MAKE) -C ../halbridge48000

fwboot:
	$(MAKE) -C ../../device/fwboot

haltransport: main.cpp build_identity.cpp ../../../version.h ../transport_status_shared.h ../../../hal/include/macfw_hal_shm.h ../../../hal/include/macfw_hal_transport_status.h fwboot
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp build_identity.cpp -o $@

clean:
	rm -f haltransport

.PHONY: all clean bridges fwboot
