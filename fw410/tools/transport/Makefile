TOOLS := amdtp44probe capturebridge48000 captureprobe cmpconnect cmpprobe coreaudiobridge coreaudiodiag formatprobe halbridge44100 halbridge48000 haltransport isocapture isoduplex isoplayback pcm44100playback pcmbufferplayback pcmstreamplayback shmprobe streamprobe
.PHONY: all clean $(TOOLS)
all: $(TOOLS)
$(TOOLS):
	$(MAKE) -C $@
clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
