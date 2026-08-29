TOOLS := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe rateprobe fw410ctl
RUNTIME := rateprobe fw410ctl

.PHONY: all clean runtime $(TOOLS)

all: $(TOOLS)
runtime: $(RUNTIME)

$(TOOLS):
	$(MAKE) -C $@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
