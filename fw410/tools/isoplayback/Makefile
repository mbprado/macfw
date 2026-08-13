CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: isoplayback

isoplayback: generated.cpp
	$(CXX) $(CXXFLAGS) generated.cpp -framework IOKit -framework CoreFoundation -o isoplayback

generated.cpp: generate.py ../isoduplex/main.cpp ../common/am824_playback.h
	python3 generate.py

clean:
	rm -f isoplayback generated.cpp
