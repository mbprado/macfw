TOOLS := amdtp44probe cmpconnect cmpprobe coreaudiobridge coreaudiodiag formatprobe isocapture isoduplex isoplayback pcm44100playback pcmbufferplayback pcmstreamplayback shmprobe streamprobe
.PHONY: all clean $(TOOLS)
all: $(TOOLS)
$(TOOLS):
	$(MAKE) -C $@
clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
