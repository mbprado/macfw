CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: pcm44100warmup script

pcm44100warmup: main.cpp ../../../lib/libmacfw.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@

script:
	chmod +x run44100warmup.sh

clean:
	rm -f pcm44100warmup

.PHONY: all clean script
