CXX := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
FRAMEWORKS := -framework IOKit -framework CoreFoundation

PROBE_DIR := ../../../fw410/tools/device/fwprobe
BOOT_TOOL := fwboot1814

.PHONY: fingerprint probe boot-tool boot-check boot clean

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

clean:
	$(MAKE) -C $(PROBE_DIR) clean
	rm -f $(BOOT_TOOL)
