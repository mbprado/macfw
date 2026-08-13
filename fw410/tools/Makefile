TOOLS := avcprobe cmpconnect cmpprobe fcpprobe formatprobe fwboot fwprobe streamprobe

.PHONY: all clean $(TOOLS)

all: $(TOOLS)

$(TOOLS):
	@echo "==> building $@"
	$(MAKE) -C $@

clean:
	@for tool in $(TOOLS); do \
		echo "==> cleaning $$tool"; \
		$(MAKE) -C $$tool clean || exit $$?; \
	done
