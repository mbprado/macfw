CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../lib/include
LDFLAGS += -framework IOKit -framework CoreFoundation -framework CoreAudio -framework AudioUnit

all: coreaudiobridge44100

lib:
	$(MAKE) -C ../../../lib libmacfw.a

rateprobe:
	$(MAKE) -C ../../control/rateprobe rateprobe

coreaudiobridge44100: main.cpp entry.cpp lib rateprobe
	perl -0pe 's/constexpr double kRequiredRate = 44100\.0;\n//; s/pcm\.availableFrames\(\) < 2048/pcm.availableFrames() < 4096/; s/capStart = true;\n\n    \/\/ Linux/capStart = true;\n\n    {\n        UInt32 liveCycleTime = 0;\n        if ((*native)->GetCycleTime(native, \&liveCycleTime) != kIOReturnSuccess) goto cleanup;\n        const UInt32 liveNow = cycleCount(liveCycleTime);\n        const UInt32 forward = (firstCycle + kCyclesPerSecond - liveNow) % kCyclesPerSecond;\n        if (forward > 4096u) {\n            std::cout << "status: FAIL - scheduled first TX cycle already missed\\n";\n            goto cleanup;\n        }\n        const double untilReassert = static_cast<double>(forward) \/ static_cast<double>(kCyclesPerSecond) + 0.020;\n        std::cout << "duplex ISO: started\\n"\n                  << "waiting " << std::fixed << std::setprecision(3) << untilReassert\n                  << " s so the reassert occurs ~20 ms after TX actually begins\\n"\n                  << std::defaultfloat;\n        CFRunLoopRunInMode(kCFRunLoopDefaultMode, untilReassert, false);\n    }\n\n    \/\/ Linux/s; s/int main\(int argc, char\*\* argv\)/int coreaudiobridge44100_inner_main(int argc, char** argv)/' main.cpp > .main.patched.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c .main.patched.cpp -o .main.patched.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c entry.cpp -o .entry.o
	$(CXX) .main.patched.o .entry.o ../../../lib/libmacfw.a $(LDFLAGS) -o $@
	rm -f .main.patched.cpp .main.patched.o .entry.o

clean:
	rm -f coreaudiobridge44100 .main.patched.cpp .main.patched.o .entry.o

.PHONY: all clean lib rateprobe
