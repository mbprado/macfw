CONTROL := avcprobe fcpprobe mixerprobe plugprobe topologyprobe headphoneprobe
TRANSPORT := cmpconnect cmpprobe formatprobe isocapture isoduplex isoplayback pcmbufferplayback streamprobe
DEVICE := fwboot fwprobe
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
