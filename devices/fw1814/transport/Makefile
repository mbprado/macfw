CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

LIBMACFW := ../../fw410/lib/libmacfw.a
LIBMACFW_INCLUDE := ../../fw410/lib/include
DEVICE_INCLUDE := ..
HAL_INCLUDE := ../hal/include

ANALOG48 := fw1814analog48
ANALOG44 := fw1814analog44
ANALOG96 := fw1814analog96
ANALOG88 := fw1814analog88
SHM_TEST := fw1814shmtest
SUPERVISOR := fw1814supervisor
DUPLEX44_PROBE := fw1814duplex44
TONE44_PROBE := fw1814tone44

.PHONY: all runtime analog48 analog44 analog96-experimental analog88-experimental shm-test supervisor duplex44-probe tone44-probe clean

all: $(ANALOG48) $(ANALOG44) $(SHM_TEST) $(SUPERVISOR) $(DUPLEX44_PROBE) $(TONE44_PROBE)

runtime: $(ANALOG48) $(ANALOG44) $(SHM_TEST) $(SUPERVISOR)

$(LIBMACFW):
	$(MAKE) -C ../../fw410/lib libmacfw.a

$(ANALOG48): analog48_main.cpp blocking_pcm_tx.h pcm_stream48.h capture_pump.h \
	duplex_lifecycle.h engine_ready.h fw1814_control_server.h playback_pump.h realtime_service.h shared_io.h \
	../channel_map.h ../fcp_control.h ../special_mixer.h ../special_mixer_model.h \
	../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		analog48_main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

$(ANALOG44): analog44_main.cpp blocking_pcm_tx44.h capture_pump44.h \
	duplex_lifecycle.h engine_ready.h fw1814_control_server.h playback_pump.h realtime_service.h shared_io.h \
	../channel_map.h ../fcp_control.h ../special_mixer.h ../special_mixer_model.h \
	../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		analog44_main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

# Explicit build only. The opt-in installer enables selection separately.
$(ANALOG96): analog96_main.cpp ../tools/fw1814_pcm96_tx.h \
	../tools/fw1814_pcm96_stream.h ../tools/fw1814_capture96_pump.h \
	duplex_lifecycle.h engine_ready.h fw1814_control_server.h playback_pump.h realtime_service.h shared_io.h \
	../channel_map.h ../fcp_control.h ../special_mixer.h ../special_mixer_model.h \
	../hal/include/macfw_fw1814_hal_shm.h ../hal/include/macfw_fw1814_capture_shm.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		analog96_main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

analog96-experimental:
	$(MAKE) -C ../../fw410/lib libmacfw.a
	$(MAKE) $(ANALOG96)

$(ANALOG88): analog88_main.cpp blocking_pcm_tx88.h \
	../tools/fw1814_capture88_pump.h ../tools/fw1814_receive_cycle.h \
	duplex_lifecycle.h engine_ready.h \
	fw1814_control_server.h playback_pump.h realtime_service.h shared_io.h \
	../channel_map.h ../fcp_control.h ../special_mixer.h ../special_mixer_model.h \
	../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		analog88_main.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

analog88-experimental:
	$(MAKE) -C ../../fw410/lib libmacfw.a
	$(MAKE) $(ANALOG88)

$(SHM_TEST): shm_test.cpp ../hal/include/macfw_fw1814_hal_shm.h \
	../hal/include/macfw_fw1814_capture_shm.h
	$(CXX) $(CXXFLAGS) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) shm_test.cpp -o $@

$(SUPERVISOR): supervisor_main.cpp ../hal/include/macfw_fw1814_hal_shm.h ../hal/include/macfw_fw1814_high_rate.h
	$(CXX) $(CXXFLAGS) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) supervisor_main.cpp -o $@

$(DUPLEX44_PROBE): ../tools/fw1814duplex44.cpp blocking_pcm_tx44.h \
	duplex_lifecycle.h realtime_service.h ../fcp_control.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		../tools/fw1814duplex44.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

$(TONE44_PROBE): ../tools/fw1814tone44.cpp blocking_pcm_tx44.h \
	duplex_lifecycle.h realtime_service.h ../channel_map.h ../fcp_control.h \
	../special_mixer.h ../special_mixer_model.h $(LIBMACFW)
	$(CXX) $(CXXFLAGS) -I$(LIBMACFW_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HAL_INCLUDE) \
		../tools/fw1814tone44.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

analog48: $(ANALOG48)

analog44: $(ANALOG44)

shm-test: $(SHM_TEST)

supervisor: $(SUPERVISOR)

duplex44-probe: $(DUPLEX44_PROBE)

tone44-probe: $(TONE44_PROBE)

clean:
	rm -f $(ANALOG48) $(ANALOG44) $(ANALOG96) $(ANALOG88) $(SHM_TEST) $(SUPERVISOR) $(DUPLEX44_PROBE) $(TONE44_PROBE)
