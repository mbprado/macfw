CATEGORIES := device transport control
LEGACY := avcprobe cmpconnect cmpprobe fcpprobe formatprobe fwboot fwprobe isocapture isoduplex isoplayback mixerprobe plugprobe streamprobe topologyprobe headphoneprobe

.PHONY: all clean $(CATEGORIES) $(LEGACY)

all: $(CATEGORIES)

device transport control:
	@echo "==> building $@ tools"
	$(MAKE) -C $@

fwboot fwprobe:
	$(MAKE) -C device $@

cmpconnect cmpprobe formatprobe isocapture isoduplex isoplayback streamprobe:
	$(MAKE) -C transport $@

avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe:
	$(MAKE) -C control $@

clean:
	@for category in $(CATEGORIES); do \
		echo "==> cleaning $$category tools"; \
		$(MAKE) -C $$category clean || exit $$?; \
	done
