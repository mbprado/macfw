PREFIX ?= /Library/Application Support/macfw/fw410

.PHONY: all clean install uninstall hal runtime all-tools

all: hal runtime

hal:
	$(MAKE) -C hal

runtime: hal
	$(MAKE) -C tools transport
	$(MAKE) -C tools rateprobe
	$(MAKE) -C tools fwboot
	$(MAKE) -C tools deviceprobe

all-tools: hal
	$(MAKE) -C tools

install:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: make install must be run as root (use sudo make install)" >&2; \
		exit 1; \
	fi
	@if [ ! -x hal/build/macfw-fw410.driver/Contents/MacOS/macfw-fw410 ] || \
	    [ ! -x tools/transport/haltransport/haltransport ] || \
	    [ ! -x tools/transport/halbridge44100/halbridge44100 ] || \
	    [ ! -x tools/transport/halbridge48000/halbridge48000 ] || \
	    [ ! -x tools/transport/transportstatus/transportstatus ] || \
	    [ ! -x tools/control/rateprobe/rateprobe ] || \
	    [ ! -x tools/device/fwboot/fwboot ] || \
	    [ ! -x tools/device/deviceprobe/deviceprobe ]; then \
		echo "error: release runtime is not built; run 'make runtime' as a normal user first" >&2; \
		exit 1; \
	fi
	$(MAKE) -C hal install
	bash service/install-service.sh

uninstall:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: make uninstall must be run as root (use sudo make uninstall)" >&2; \
		exit 1; \
	fi
	bash service/uninstall-service.sh
	$(MAKE) -C hal uninstall

clean:
	$(MAKE) -C lib clean
	$(MAKE) -C hal clean
	$(MAKE) -C tools clean
