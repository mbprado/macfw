CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../hal/include

all: transportstatus

transportstatus: main.cpp ../../../hal/include/macfw_hal_transport_status.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

clean:
	rm -f transportstatus

.PHONY: all clean
