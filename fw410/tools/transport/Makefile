TOOLS := cmpconnect cmpprobe formatprobe isocapture isoduplex isoplayback streamprobe

.PHONY: all clean $(TOOLS)

all: $(TOOLS)

$(TOOLS):
	$(MAKE) -C ../$@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C ../$$tool clean || exit $$?; done
