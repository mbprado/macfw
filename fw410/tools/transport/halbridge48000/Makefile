CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include -I../../../hal/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: halbridge48000

lib:
	$(MAKE) -C ../../../lib libmacfw.a

rateprobe:
	$(MAKE) -C ../../control/rateprobe rateprobe

halbridge48000: main.cpp entry.cpp lib rateprobe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp entry.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

clean:
	rm -f halbridge48000

.PHONY: all clean lib rateprobe
