TOOLS := amdtp44probe capturebridge48000 captureprobe cmpconnect cmpprobe coreaudiobridge coreaudiodiag formatprobe halbridge44100 halbridge48000 haltransport isocapture isoduplex isoplayback pcm44100playback pcmbufferplayback pcmstreamplayback shmprobe streamprobe transportstatus
RUNTIME := halbridge44100 halbridge48000 haltransport transportstatus

.PHONY: all clean runtime transport transport-clean $(TOOLS)

all: $(TOOLS)
runtime transport: $(RUNTIME)

$(TOOLS):
	$(MAKE) -C $@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done

transport-clean:
	rm -f halbridge48000/halbridge48000 halbridge44100/halbridge44100 haltransport/haltransport transportstatus/transportstatus
