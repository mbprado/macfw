TOOLS := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe rateprobe fw410ctl
.PHONY: all clean $(TOOLS)
all: $(TOOLS)
$(TOOLS):
	$(MAKE) -C $@
clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
