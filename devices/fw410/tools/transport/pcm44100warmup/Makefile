CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation

all: pcm44100warmup script

pcm44100warmup: main.cpp ../../../lib/libmacfw.a
	perl -pe 's/if \(\(\*responseSpace_\)->TurnOnNotification\(responseSpace_\) != kIOReturnSuccess\) \{/if (!(*responseSpace_)->TurnOnNotification(responseSpace_)) {/' main.cpp > .main.patched.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) .main.patched.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@
	rm -f .main.patched.cpp

script:
	chmod +x run44100warmup.sh

clean:
	rm -f pcm44100warmup .main.patched.cpp

.PHONY: all clean script
