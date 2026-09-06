CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

LIBMACFW := ../../../fw410/lib/libmacfw.a
LIBMACFW_INCLUDE := ../../../fw410/lib/include
DEVICE_INCLUDE := ..
HAL_INCLUDE := ../hal/include

ANALOG48 := fw1814analog48
SHM_TEST := fw1814shmtest

.PHONY: all analog48 shm-test clean

all: $(ANALOG48) $(SHM_TEST)

$(LIBMACFW):
	$(MAKE) -C ../../../fw410/lib libmacfw.a

$(ANALOG48): analog48_main.cpp blocking_pcm_tx.h pcm_stream48.h capture_pump.h \
	duplex_lifecycle.h playback_pump.h shared_io.h ../channel_map.h ../fcp_control.h \
	../special_mixer.h ../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		analog48_main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

$(SHM_TEST): shm_test.cpp ../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h
	$(CXX) $(CXXFLAGS) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) shm_test.cpp -o $@

analog48: $(ANALOG48)

shm-test: $(SHM_TEST)

clean:
	rm -f $(ANALOG48) $(SHM_TEST)
