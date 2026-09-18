CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

PROBE_DIR := ../../fw410/tools/device/fwprobe
BEBOB_PROBE_DIR := ../../../common/tools/bebobprobe
BUS_RESET_DIR := ../../../common/tools/firewirebusreset
LIBMACFW := ../../fw410/lib/libmacfw.a
LIBMACFW_INCLUDE := ../../fw410/lib/include
BOOT_TOOL := fwboot1814
INIT_TOOL := fw1814init
CAPTURE48_TOOL := fw1814capture48
DUALCMP_TOOL := fw1814capture48_dualcmp
DUALCMP_WAIT_TOOL := fw1814capture48_dualcmp_wait
DUPLEX_BLOCKING_TOOL := fw1814capture48_duplex_blocking
DUPLEX96_TOOL := fw1814capture96_duplex_blocking
DUPLEX88_TOOL := fw1814capture88_duplex_blocking
TONE96_TOOL := fw1814tone96
TONE96_LIVE_TOOL := fw1814tone96_live
OUTPUT_WATCH_TOOL := fw1814capture48_output_watch
TEARDOWN_WATCH_TOOL := fw1814teardown_watch
CLIENT_CLOSE_WATCH_TOOL := fw1814client_close_watch
STREAM_CLIENT_CLOSE_WATCH_TOOL := fw1814stream_client_close_watch
PLAYBACK_MAP_TOOL := fw1814playback_map
ROUTE_ANALOG_TOOL := fw1814route_stream_to_analog
CONTROL_DIR := control/fw1814ctl
STATE_CONTROL_DIR := control/fw1814state
DEVICE_PROBE_TOOL := fw1814deviceprobe

.PHONY: all runtime fingerprint probe boot-tool boot-check boot \
	bebob-probe bus-reset-tool bus-reset-check bus-reset operational-probe \
	init-tool init-check init-48 init-44 init-88-check init-96-check \
	capture48-tool capture48-check capture48 capture48-raw \
	dualcmp-tool dualcmp-check dualcmp dualcmp-raw \
	dualcmp-wait-tool dualcmp-wait-check dualcmp-wait dualcmp-wait-raw \
	duplex-blocking-tool duplex-blocking-check duplex-blocking duplex-blocking-raw \
	duplex96-tool duplex96-check duplex96 duplex96-raw \
	duplex88-tool duplex88-check duplex88 duplex88-raw schedule88-check \
	tone96-tool tone96-check tone96 tone96-raw \
	tone96-live-tool tone96-live-check tone96-live \
	output-watch-tool output-watch-check output-watch output-watch-raw \
	teardown-watch-tool teardown-watch-check teardown-watch \
	client-close-watch-tool client-close-watch-check client-close-watch \
	stream-client-close-watch-tool stream-client-close-watch-check stream-client-close-watch \
	playback-map-tool playback-map-check playback-map \
	route-analog-tool route-analog-check route-analog control-tool control-state-tool device-probe-tool clean

all: probe bebob-probe bus-reset-tool boot-tool init-tool capture48-tool dualcmp-tool dualcmp-wait-tool \
	duplex-blocking-tool output-watch-tool teardown-watch-tool \
	client-close-watch-tool stream-client-close-watch-tool playback-map-tool \
	route-analog-tool control-tool control-state-tool

runtime: boot-tool init-tool bus-reset-tool control-tool control-state-tool device-probe-tool

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

# Read-only preflight for high-rate CONTROL diagnostics. Execution requires
# an explicit --experimental-high-rate option with the transport stopped.
init-88-check: init-tool
	./$(INIT_TOOL) 88200

init-96-check: init-tool
	./$(INIT_TOOL) 96000

$(LIBMACFW):
	$(MAKE) -C ../../fw410/lib libmacfw.a

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

# Experimental 96 kHz duplex packet probe. This is not part of the normal
# build or production transport; execution requires an explicit target.
duplex96-tool: $(DUPLEX96_TOOL)

$(DUPLEX96_TOOL): fw1814capture96_duplex_nodata.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814capture96_duplex_nodata.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

duplex96-check: duplex96-tool
	./$(DUPLEX96_TOOL)

duplex96: duplex96-tool
	./$(DUPLEX96_TOOL) --execute --experimental-high-rate

duplex96-raw: duplex96-tool
	./$(DUPLEX96_TOOL) --execute --experimental-high-rate --raw

# Guarded 88.2 kHz duplex probe with a live 640/320-slot TX refill. Like the
# 96 kHz probe, it is excluded from the default build and installed service.
duplex88-tool: $(DUPLEX88_TOOL)

$(DUPLEX88_TOOL): fw1814capture88_duplex_nodata.cpp fw1814_capture88_pump.h ../transport/blocking_pcm_tx88.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814capture88_duplex_nodata.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

duplex88-check: duplex88-tool
	./$(DUPLEX88_TOOL)

duplex88: duplex88-tool
	./$(DUPLEX88_TOOL) --execute --experimental-high-rate

duplex88-raw: duplex88-tool
	./$(DUPLEX88_TOOL) --execute --experimental-high-rate --raw

# Offline proof that all immutable NuDCL send lengths match through the full
# base-44.1 scheduler phase and that DBC continues through ring reuse.
schedule88-check: test_88200_schedule.cpp
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) $< -o /tmp/macfw-fw1814-schedule88-check
	/tmp/macfw-fw1814-schedule88-check

# Experimental 96 kHz, phase-aligned 500 Hz tone in one playback position.
# Uses the proven duplex probe's FCP reply matching and restore checks.
tone96-tool: $(TONE96_TOOL)

$(TONE96_TOOL): fw1814tone96.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814tone96.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

tone96-check: tone96-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make tone96-check POSITION=0..5"; exit 64; fi
	./$(TONE96_TOOL) --position $(POSITION)

tone96: tone96-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make tone96 POSITION=0..5"; exit 64; fi
	./$(TONE96_TOOL) --position $(POSITION) --execute --experimental-high-rate

tone96-raw: tone96-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make tone96-raw POSITION=0..5"; exit 64; fi
	./$(TONE96_TOOL) --position $(POSITION) --execute --experimental-high-rate --raw

# Live PCM refill probe: frequencies need not fit the 128-cycle static ring.
# FCP waits service the refill loop; this does not install a 96 kHz HAL.
# The archive lives in a separate Makefile. Refresh it before checking the
# executable's timestamp or a changed library object can leave an old tool.
tone96-live-tool:
	$(MAKE) -C ../../fw410/lib libmacfw.a
	$(MAKE) $(TONE96_LIVE_TOOL)

$(TONE96_LIVE_TOOL): fw1814tone96_live.cpp fw1814_pcm96_tx.h fw1814_pcm96_stream.h fw1814_capture96_pump.h fw1814_file96.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814tone96_live.cpp $(LIBMACFW) $(FRAMEWORKS) -framework AudioToolbox -o $@

tone96-live-check: tone96-live-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make tone96-live-check POSITION=0..5 FREQUENCY=440"; exit 64; fi
	./$(TONE96_LIVE_TOOL) --position $(POSITION) --frequency $(or $(FREQUENCY),440)

tone96-live: tone96-live-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make tone96-live POSITION=0..5 FREQUENCY=440"; exit 64; fi
	./$(TONE96_LIVE_TOOL) --position $(POSITION) --frequency $(or $(FREQUENCY),440) --execute --experimental-high-rate

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

# Stepwise teardown diagnostic. It starts the same blocking duplex transport
# but sends no AV/C rate CONTROL at all. Bus generation is sampled after each
# teardown operation so we can locate the first operation that triggers the
# observed generation increment.
teardown-watch-tool: $(TEARDOWN_WATCH_TOOL)

$(TEARDOWN_WATCH_TOOL): fw1814teardown_watch.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814teardown_watch.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

teardown-watch-check: teardown-watch-tool
	./$(TEARDOWN_WATCH_TOOL)

teardown-watch: teardown-watch-tool
	./$(TEARDOWN_WATCH_TOOL) --execute

# Plain client lifecycle diagnostic. It uses a second read-only observer handle
# and performs no ISO, CMP or AV/C operations. This isolates whether simply
# closing/releasing an IOFireWireLib client changes the bus generation.
client-close-watch-tool: $(CLIENT_CLOSE_WATCH_TOOL)

$(CLIENT_CLOSE_WATCH_TOOL): fw1814client_close_watch.cpp $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814client_close_watch.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

client-close-watch-check: client-close-watch-tool
	./$(CLIENT_CLOSE_WATCH_TOOL)

client-close-watch: client-close-watch-tool
	./$(CLIENT_CLOSE_WATCH_TOOL) --execute

# Same lifecycle observation after a complete blocking duplex ISO session.
# A separate unopened observer handle remains alive while the stream client
# tears down notifications, dispatcher, Close(), and interface/plugin/service.
# No AV/C rate CONTROL is sent.
stream-client-close-watch-tool: $(STREAM_CLIENT_CLOSE_WATCH_TOOL)

$(STREAM_CLIENT_CLOSE_WATCH_TOOL): fw1814stream_client_close_watch.cpp fw1814_blocking_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814stream_client_close_watch.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

stream-client-close-watch-check: stream-client-close-watch-tool
	./$(STREAM_CLIENT_CLOSE_WATCH_TOOL)

stream-client-close-watch: stream-client-close-watch-tool
	./$(STREAM_CLIENT_CLOSE_WATCH_TOOL) --execute

# Guarded physical playback-position mapper. It reuses the proven full-duplex
# 48 kHz startup and replaces playback silence with a 500 Hz / -24 dBFS tone
# in one explicitly selected PCM position. Positions 0..5 are tested one at a
# time; no physical output name is assumed in software.
playback-map-tool: $(PLAYBACK_MAP_TOOL)

$(PLAYBACK_MAP_TOOL): fw1814playback_map.cpp fw1814_tone_tx.h $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) fw1814playback_map.cpp $(LIBMACFW) $(FRAMEWORKS) -o $@

playback-map-check: playback-map-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make playback-map-check POSITION=0..5"; exit 64; fi
	./$(PLAYBACK_MAP_TOOL) --position $(POSITION)

playback-map: playback-map-tool
	@if [ -z "$(POSITION)" ]; then echo "usage: make playback-map POSITION=0..5"; exit 64; fi
	./$(PLAYBACK_MAP_TOOL) --position $(POSITION) --execute

# Minimal documented special-firmware routing write used only to make the four
# analog playback stream positions reachable at the physical analog jacks.
# The FW1814 mixer register area is write-only; no readback/probing is attempted.
route-analog-tool: $(ROUTE_ANALOG_TOOL)

$(ROUTE_ANALOG_TOOL): fw1814route_stream_to_analog.cpp $(LIBMACFW)
	$(CXX) -I$(LIBMACFW_INCLUDE) $(CXXFLAGS) $< $(LIBMACFW) $(FRAMEWORKS) -o $@

route-analog-check: route-analog-tool
	./$(ROUTE_ANALOG_TOOL)

route-analog: route-analog-tool
	./$(ROUTE_ANALOG_TOOL) --execute

control-tool:
	$(MAKE) -C $(CONTROL_DIR)

control-state-tool:
	$(MAKE) -C $(STATE_CONTROL_DIR)

device-probe-tool: $(DEVICE_PROBE_TOOL)

$(DEVICE_PROBE_TOOL): fw1814deviceprobe.cpp
	$(CXX) $(CXXFLAGS) $< $(FRAMEWORKS) -o $@

clean:
	$(MAKE) -C $(PROBE_DIR) clean
	$(MAKE) -C $(BEBOB_PROBE_DIR) clean
	$(MAKE) -C $(BUS_RESET_DIR) clean
	$(MAKE) -C $(CONTROL_DIR) clean
	$(MAKE) -C $(STATE_CONTROL_DIR) clean
	rm -f $(BOOT_TOOL) $(INIT_TOOL) $(DEVICE_PROBE_TOOL) $(CAPTURE48_TOOL) $(DUALCMP_TOOL) $(DUALCMP_WAIT_TOOL) $(DUPLEX_BLOCKING_TOOL) $(DUPLEX96_TOOL) $(TONE96_TOOL) $(TONE96_LIVE_TOOL) $(OUTPUT_WATCH_TOOL) $(TEARDOWN_WATCH_TOOL) $(CLIENT_CLOSE_WATCH_TOOL) $(STREAM_CLIENT_CLOSE_WATCH_TOOL) $(PLAYBACK_MAP_TOOL) $(ROUTE_ANALOG_TOOL) fw1814capture48_duplex_nodata
