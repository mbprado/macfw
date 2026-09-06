CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

PROBE_DIR := ../../../fw410/tools/device/fwprobe
BEBOB_PROBE_DIR := ../../../common/tools/bebobprobe
BUS_RESET_DIR := ../../../common/tools/firewirebusreset
LIBMACFW := ../../../fw410/lib/libmacfw.a
LIBMACFW_INCLUDE := ../../../fw410/lib/include
BOOT_TOOL := fwboot1814
INIT_TOOL := fw1814init
CAPTURE48_TOOL := fw1814capture48

.PHONY: fingerprint probe boot-tool boot-check boot \
	bebob-probe bus-reset-tool bus-reset-check bus-reset operational-probe \
	init-tool init-check init-48 init-44 \
	capture48-tool capture48-check capture48 capture48-raw clean

probe:
	$(MAKE) -C $(PROBE_DIR)

fingerprint: probe
	sh ./fingerprint.sh

boot-tool: $(BOOT_TOOL)

$(BOOT_TOOL): fwboot1814.cpp
	$(CXX) $(CXXFLAGS) $< $(FRAMEWORKS) -o $@

boot-check: boot-tool
	./$(BOOT_TOOL)

boot: boot-tool
	./$(BOOT_TOOL) --execute

bebob-probe:
	$(MAKE) -C $(BEBOB_PROBE_DIR)

bus-reset-tool:
	$(MAKE) -C $(BUS_RESET_DIR)

bus-reset-check: bus-reset-tool
	$(BUS_RESET_DIR)/firewirebusreset --product "FW 1814"

bus-reset: bus-reset-tool
	$(BUS_RESET_DIR)/firewirebusreset --product "FW 1814" --execute

operational-probe: bebob-probe
	$(BEBOB_PROBE_DIR)/bebobprobe --product "FW 1814" --maudio-special

init-tool: $(INIT_TOOL)

$(INIT_TOOL): fw1814init.cpp
	$(CXX) $(CXXFLAGS) $< $(FRAMEWORKS) -o $@

init-check: init-tool
	./$(INIT_TOOL) 48000

init-48: init-tool
	./$(INIT_TOOL) 48000 --execute

init-44: init-tool
	./$(INIT_TOOL) 44100 --execute

$(LIBMACFW):
	$(MAKE) -C ../../../fw410/lib libmacfw.a

capture48-tool: $(CAPTURE48_TOOL)

$(CAPTURE48_TOOL): fw1814capture48.cpp $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) $< $(LIBMACFW) $(FRAMEWORKS) -o $@

# FW1814 M-Audio special firmware does not start transmitting merely when
# oPCR[0] is connected. The device-specific harness starts host RX, then
# reasserts OUTPUT 48 kHz, waits 100 ms, and reasserts INPUT 48 kHz.
# The expected S/PDIF formation is 10 PCM + 1 MIDI (DBS=11), max 272 bytes.
capture48-check: capture48-tool
	./$(CAPTURE48_TOOL)

capture48: capture48-tool
	./$(CAPTURE48_TOOL) --execute

capture48-raw: capture48-tool
	./$(CAPTURE48_TOOL) --execute --raw

clean:
	$(MAKE) -C $(PROBE_DIR) clean
	$(MAKE) -C $(BEBOB_PROBE_DIR) clean
	$(MAKE) -C $(BUS_RESET_DIR) clean
	rm -f $(BOOT_TOOL) $(INIT_TOOL) $(CAPTURE48_TOOL)
