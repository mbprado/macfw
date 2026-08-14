TOOLS := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe

.PHONY: all clean $(TOOLS)

all: $(TOOLS)

avcprobe fcpprobe:
	$(MAKE) -C ../$@

mixerprobe plugprobe topologyprobe headphoneprobe:
	$(MAKE) -C $@

clean:
	$(MAKE) -C ../avcprobe clean
	$(MAKE) -C ../fcpprobe clean
	@for tool in mixerprobe plugprobe topologyprobe headphoneprobe; do $(MAKE) -C $$tool clean || exit $$?; done
