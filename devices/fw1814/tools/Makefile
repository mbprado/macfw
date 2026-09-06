.PHONY: fingerprint probe clean

PROBE_DIR := ../../../fw410/tools/device/fwprobe

probe:
	$(MAKE) -C $(PROBE_DIR)

fingerprint: probe
	sh ./fingerprint.sh

clean:
	$(MAKE) -C $(PROBE_DIR) clean
