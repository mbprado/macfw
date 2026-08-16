CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: pcmstream44100

lib:
	$(MAKE) -C ../../../lib libmacfw.a

rateprobe:
	$(MAKE) -C ../../control/rateprobe rateprobe

pcmstream44100: main.cpp entry.cpp lib rateprobe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp entry.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

clean:
	rm -f pcmstream44100

.PHONY: all clean lib rateprobe
