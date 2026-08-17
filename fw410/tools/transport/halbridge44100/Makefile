CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include -I../../../hal/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: halbridge44100

lib:
	$(MAKE) -C ../../../lib libmacfw.a

rateprobe:
	$(MAKE) -C ../../control/rateprobe rateprobe

halbridge44100: main.cpp entry.cpp lib rateprobe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp entry.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

clean:
	rm -f halbridge44100

.PHONY: all clean lib rateprobe
