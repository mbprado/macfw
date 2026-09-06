CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

PROBE_DIR := ../../../fw410/tools/device/fwprobe
BEBOB_PROBE_DIR := ../../../common/tools/bebobprobe
BUS_RESET_DIR := ../../../common/tools/firewirebusreset
AMDTP_CAPTURE_DIR := ../../../common/tools/amdtpcaptureprobe
BOOT_TOOL := fwboot1814
INIT_TOOL := fw1814init

.PHONY: fingerprint probe boot-tool boot-check boot \
	bebob-probe bus-reset-tool bus-reset-check bus-reset operational-probe \
	init-tool init-check init-48 init-44 \
	amdtp-capture-tool capture48-check capture48 capture48-raw clean

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

amdtp-capture-tool:
	$(MAKE) -C $(AMDTP_CAPTURE_DIR)

# FW1814 special-firmware S/PDIF formation at 44.1/48 kHz:
# device -> host = 10 PCM positions + 1 MIDI position, therefore DBS=11.
# At 48 kHz the maximum packet is 8-byte CIP + 6 * 11 * 4 = 272 bytes.
capture48-check: amdtp-capture-tool
	$(AMDTP_CAPTURE_DIR)/amdtpcaptureprobe --product "FW 1814" --dbs 11 --payload 272

capture48: amdtp-capture-tool
	$(AMDTP_CAPTURE_DIR)/amdtpcaptureprobe --product "FW 1814" --dbs 11 --payload 272 --execute

capture48-raw: amdtp-capture-tool
	$(AMDTP_CAPTURE_DIR)/amdtpcaptureprobe --product "FW 1814" --dbs 11 --payload 272 --execute --raw

clean:
	$(MAKE) -C $(PROBE_DIR) clean
	$(MAKE) -C $(BEBOB_PROBE_DIR) clean
	$(MAKE) -C $(BUS_RESET_DIR) clean
	$(MAKE) -C $(AMDTP_CAPTURE_DIR) clean
	rm -f $(BOOT_TOOL) $(INIT_TOOL)
