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
DUALCMP_TOOL := fw1814capture48_dualcmp
DUALCMP_WAIT_TOOL := fw1814capture48_dualcmp_wait
DUPLEX_BLOCKING_TOOL := fw1814capture48_duplex_blocking
OUTPUT_WATCH_TOOL := fw1814capture48_output_watch

.PHONY: fingerprint probe boot-tool boot-check boot \
	bebob-probe bus-reset-tool bus-reset-check bus-reset operational-probe \
	init-tool init-check init-48 init-44 \
	capture48-tool capture48-check capture48 capture48-raw \
	dualcmp-tool dualcmp-check dualcmp dualcmp-raw \
	dualcmp-wait-tool dualcmp-wait-check dualcmp-wait dualcmp-wait-raw \
	duplex-blocking-tool duplex-blocking-check duplex-blocking duplex-blocking-raw \
	output-watch-tool output-watch-check output-watch output-watch-raw clean

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

# Original receive-only experiment. This intentionally connects only oPCR[0].
capture48-check: capture48-tool
	./$(CAPTURE48_TOOL)

capture48: capture48-tool
	./$(CAPTURE48_TOOL) --execute

capture48-raw: capture48-tool
	./$(CAPTURE48_TOOL) --execute --raw

# Incremental FW1814 startup diagnostic: both CMP plugs connected, capture DMA
# only. No host->device packets are sent.
dualcmp-tool: $(DUALCMP_TOOL)

$(DUALCMP_TOOL): fw1814capture48_dualcmp.cpp $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) $< $(LIBMACFW) $(FRAMEWORKS) -o $@

dualcmp-check: dualcmp-tool
	./$(DUALCMP_TOOL)

dualcmp: dualcmp-tool
	./$(DUALCMP_TOOL) --execute

dualcmp-raw: dualcmp-tool
	./$(DUALCMP_TOOL) --execute --raw

# Same dual-CMP/capture-only experiment without an immediate STATUS readback.
dualcmp-wait-tool: $(DUALCMP_WAIT_TOOL)

$(DUALCMP_WAIT_TOOL): fw1814capture48_dualcmp_wait.cpp $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) $< $(LIBMACFW) $(FRAMEWORKS) -o $@

dualcmp-wait-check: dualcmp-wait-tool
	./$(DUALCMP_WAIT_TOOL)

dualcmp-wait: dualcmp-wait-tool
	./$(DUALCMP_WAIT_TOOL) --execute

dualcmp-wait-raw: dualcmp-wait-tool
	./$(DUALCMP_WAIT_TOOL) --execute --raw

# Full BeBoB transport-start diagnostic. Linux initializes these streams with
# CIP_BLOCKING, so 48 kHz uses three 8-event data packets followed by one
# NODATA packet. Playback is 6 PCM + 1 MIDI (DBS=7, max 232 bytes); capture is
# 10 PCM + 1 MIDI (DBS=11, max 360 bytes). Playback starts first, then capture,
# then the M-Audio OUTPUT -> 100 ms -> INPUT rate kick.
duplex-blocking-tool: $(DUPLEX_BLOCKING_TOOL)

$(DUPLEX_BLOCKING_TOOL): fw1814capture48_duplex_nodata.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814capture48_duplex_nodata.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

duplex-blocking-check: duplex-blocking-tool
	./$(DUPLEX_BLOCKING_TOOL)

duplex-blocking: duplex-blocking-tool
	./$(DUPLEX_BLOCKING_TOOL) --execute

duplex-blocking-raw: duplex-blocking-tool
	./$(DUPLEX_BLOCKING_TOOL) --execute --raw

# Safety diagnostic for the observed generation change during duplex startup.
# It starts the same correct blocking streams, sends OUTPUT 48 kHz only, and
# watches the FireWire bus generation for 300 ms. INPUT CONTROL is never sent.
# If a reset is detected, stale-generation PCR restore writes are skipped.
output-watch-tool: $(OUTPUT_WATCH_TOOL)

$(OUTPUT_WATCH_TOOL): fw1814capture48_output_watch.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814capture48_output_watch.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

output-watch-check: output-watch-tool
	./$(OUTPUT_WATCH_TOOL)

output-watch: output-watch-tool
	./$(OUTPUT_WATCH_TOOL) --execute

output-watch-raw: output-watch-tool
	./$(OUTPUT_WATCH_TOOL) --execute --raw

clean:
	$(MAKE) -C $(PROBE_DIR) clean
	$(MAKE) -C $(BEBOB_PROBE_DIR) clean
	$(MAKE) -C $(BUS_RESET_DIR) clean
	rm -f $(BOOT_TOOL) $(INIT_TOOL) $(CAPTURE48_TOOL) $(DUALCMP_TOOL) $(DUALCMP_WAIT_TOOL) $(DUPLEX_BLOCKING_TOOL) $(OUTPUT_WATCH_TOOL) fw1814capture48_duplex_nodata
