CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include -I../../../hal/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: capturebridge48000

lib:
	$(MAKE) -C ../../../lib libmacfw.a

capturebridge48000: main.cpp ../capture_shared.h lib
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

clean:
	rm -f capturebridge48000

.PHONY: all clean lib
