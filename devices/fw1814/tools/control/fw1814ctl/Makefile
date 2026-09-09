CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic

all: fw1814ctl

fw1814ctl: main.cpp ../../../special_mixer_model.h
	$(CXX) $(CXXFLAGS) main.cpp -o $@

model_test: model_test.cpp ../../../special_mixer_model.h
	$(CXX) $(CXXFLAGS) model_test.cpp -o $@

check: fw1814ctl model_test
	./model_test

clean:
	rm -f fw1814ctl model_test

.PHONY: all check clean
