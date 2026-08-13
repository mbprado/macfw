CXX ?= clang++
AR ?= ar
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude
FRAMEWORKS := -framework IOKit -framework CoreFoundation

LIB := libmacfw.a
LIB_OBJS := src/firewire_device.o src/cmp.o src/isoch_allocation.o

all: $(LIB) smoke

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

src/firewire_device.o: src/firewire_device.cpp include/macfw/firewire_device.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

src/cmp.o: src/cmp.cpp include/macfw/cmp.h include/macfw/firewire_device.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

src/isoch_allocation.o: src/isoch_allocation.cpp include/macfw/isoch_allocation.h include/macfw/firewire_device.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

smoke: src/smoke.cpp include/macfw/channel_map.h include/macfw/am824.h include/macfw/amdtp_packet.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/smoke.cpp -o $@

check: $(LIB) smoke
	./smoke

clean:
	rm -f smoke $(LIB) $(LIB_OBJS)

.PHONY: all check clean
