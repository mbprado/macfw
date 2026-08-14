TOOLS := fwboot fwprobe

.PHONY: all clean $(TOOLS)

all: $(TOOLS)

$(TOOLS):
	$(MAKE) -C ../$@

clean:
	@for tool in $(TOOLS); do $(MAKE) -C ../$$tool clean || exit $$?; done
