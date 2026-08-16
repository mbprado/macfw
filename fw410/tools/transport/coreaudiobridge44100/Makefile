CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioUnit

all: coreaudiobridge44100 script

../../../lib/libmacfw.a:
	$(MAKE) -C ../../../lib libmacfw.a

coreaudiobridge44100: main.cpp ../../../lib/libmacfw.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

script:
	chmod +x run44100.sh

clean:
	rm -f coreaudiobridge44100

.PHONY: all clean script
