CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS += -I../../../hal/include

all: transportstatus transportstatus-reset

transportstatus: main.cpp ../../../hal/include/macfw_hal_transport_status.h ../../../hal/include/macfw_hal_capture_shm.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp -o $@

transportstatus-reset: reset.cpp ../../../hal/include/macfw_hal_transport_status.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) reset.cpp -o $@

clean:
	rm -f transportstatus transportstatus-reset

.PHONY: all clean
