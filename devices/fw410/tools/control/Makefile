TOOLS := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe rateprobe fw410ctl fw410state
RUNTIME := rateprobe fw410ctl fw410state

.PHONY: all clean runtime $(TOOLS)

all: $(TOOLS)
runtime: $(RUNTIME)

$(TOOLS):
	$(MAKE) -C $@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
