TOOLS := fwboot fwprobe deviceprobe
RUNTIME := fwboot deviceprobe

.PHONY: all clean runtime $(TOOLS)

all: $(TOOLS)
runtime: $(RUNTIME)

$(TOOLS):
	$(MAKE) -C $@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C $$tool clean || exit $$?; done
