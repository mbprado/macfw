CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS := -I../../lib/include
FRAMEWORKS := -framework IOKit -framework CoreFoundation
TARGET := cmpconnect
SOURCES := main.cpp
LIBMACFW := ../../lib/libmacfw.a

.PHONY: all clean libmacfw

all: $(TARGET)

libmacfw:
	$(MAKE) -C ../../lib libmacfw.a

$(TARGET): $(SOURCES) $(LIBMACFW)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LIBMACFW) $(FRAMEWORKS) -o $(TARGET)

$(LIBMACFW):
	$(MAKE) -C ../../lib libmacfw.a

clean:
	rm -f $(TARGET)
