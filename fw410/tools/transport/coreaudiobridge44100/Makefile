CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioUnit

all: coreaudiobridge44100 script

lib:
	$(MAKE) -C ../../../lib libmacfw.a

coreaudiobridge44100: main.cpp lib
	perl -0pe 's/constexpr double kRequiredRate = 44100\.0;\n//; s/capStart = true;\n\n    \/\/ Linux/capStart = true;\n\n    {\n        UInt32 liveCycleTime = 0;\n        if ((*native)->GetCycleTime(native, \&liveCycleTime) != kIOReturnSuccess) goto cleanup;\n        const UInt32 liveNow = cycleCount(liveCycleTime);\n        const UInt32 forward = (firstCycle + kCyclesPerSecond - liveNow) % kCyclesPerSecond;\n        if (forward > 4096u) {\n            std::cout << "status: FAIL - scheduled first TX cycle already missed\\n";\n            goto cleanup;\n        }\n        const double untilReassert = static_cast<double>(forward) \/ static_cast<double>(kCyclesPerSecond) + 0.020;\n        std::cout << "duplex ISO: started\\n"\n                  << "waiting " << std::fixed << std::setprecision(3) << untilReassert\n                  << " s so the reassert occurs ~20 ms after TX actually begins\\n"\n                  << std::defaultfloat;\n        CFRunLoopRunInMode(kCFRunLoopDefaultMode, untilReassert, false);\n    }\n\n    \/\/ Linux/s' main.cpp > .main.patched.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) .main.patched.cpp ../../../lib/libmacfw.a $(LDFLAGS) -o $@
	rm -f .main.patched.cpp

script:
	chmod +x run44100.sh

clean:
	rm -f coreaudiobridge44100 .main.patched.cpp

.PHONY: all clean script lib
