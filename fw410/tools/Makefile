CONTROL := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe rateprobe fw410ctl
TRANSPORT := capturebridge48000 captureprobe cmpconnect cmpprobe coreaudiobridge coreaudiodiag formatprobe halbridge44100 halbridge48000 haltransport isocapture isoduplex isoplayback pcm44100playback pcmbufferplayback pcmstreamplayback shmprobe streamprobe amdtp44probe transportstatus
DEVICE := fwboot fwprobe deviceprobe
TOOLS := $(CONTROL) $(TRANSPORT) $(DEVICE)

.PHONY: all clean control transport device $(TOOLS)

all: control transport device
control:
	$(MAKE) -C control
transport:
	$(MAKE) -C transport
device:
	$(MAKE) -C device

$(CONTROL):
	$(MAKE) -C control/$@
$(TRANSPORT):
	$(MAKE) -C transport/$@
$(DEVICE):
	$(MAKE) -C device/$@

clean:
	$(MAKE) -C control clean
	$(MAKE) -C transport clean
	$(MAKE) -C device clean
