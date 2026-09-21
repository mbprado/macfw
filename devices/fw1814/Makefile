PREFIX ?= /Library/Application Support/macfw/fw1814

.PHONY: all clean install-check install install-experimental96 install-experimental88 install-experimental176 uninstall hal runtime all-tools tools gui

all: hal runtime gui

hal:
	$(MAKE) -C hal all

runtime:
	$(MAKE) -C transport runtime
	$(MAKE) -C tools runtime

gui:
	$(MAKE) -C control-panel all

all-tools tools:
	$(MAKE) -C tools all

install-check:
	@if [ ! -x hal/build/macfw-fw1814.driver/Contents/MacOS/macfw-fw1814 ] || \
	    [ ! -x transport/fw1814supervisor ] || \
	    [ ! -x transport/fw1814analog48 ] || \
	    [ ! -x transport/fw1814analog44 ] || \
	    [ ! -x tools/fw1814init ] || \
	    [ ! -x tools/fwboot1814 ] || \
	    [ ! -x tools/fw1814firmwarereset ] || \
	    [ ! -x tools/fw1814deviceprobe ] || \
	    [ ! -x ../../common/tools/firewirebusreset/firewirebusreset ] || \
	    [ ! -x tools/control/fw1814ctl/fw1814ctl ] || \
	    [ ! -x tools/control/fw1814state/fw1814state ] || \
	    [ ! -x control-panel/build/macfw-fw1814-control.app/Contents/MacOS/macfw-fw1814-control ]; then \
		echo "error: installable FW1814 artifacts are not built; run 'make fw1814' as a normal user first" >&2; \
		exit 1; \
	fi

install: install-check
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: FW1814 install must be run as root (use sudo make fw1814-install)" >&2; \
		exit 1; \
	fi
	bash service/install-service.sh --check-only
	$(MAKE) -C hal install
	bash service/install-service.sh
	$(MAKE) -C control-panel install

# All artifacts and hardware are checked before the HAL or service is replaced.
install-experimental96: install-check
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: experimental FW1814 install must be run as root" >&2; exit 1; \
	fi
	@if [ ! -x transport/fw1814analog96 ]; then \
		echo "error: build the 96 kHz engine with 'make fw1814-experimental96' first" >&2; exit 1; \
	fi
	MACFW_INSTALL_EXPERIMENTAL96=1 bash service/install-service.sh --check-only
	$(MAKE) -C hal install
	MACFW_INSTALL_EXPERIMENTAL96=1 bash service/install-service.sh
	$(MAKE) -C control-panel install

install-experimental88: install-check
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: experimental FW1814 install must be run as root" >&2; exit 1; \
	fi
	@if [ ! -x transport/fw1814analog88 ]; then \
		echo "error: build the 88.2 kHz engine with 'make fw1814-experimental88' first" >&2; exit 1; \
	fi
	MACFW_INSTALL_EXPERIMENTAL88=1 bash service/install-service.sh --check-only
	$(MAKE) -C hal install
	MACFW_INSTALL_EXPERIMENTAL88=1 bash service/install-service.sh
	$(MAKE) -C control-panel install

install-experimental176: install-check
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: experimental FW1814 install must be run as root" >&2; exit 1; \
	fi
	@if [ ! -x transport/fw1814analog176 ]; then \
		echo "error: build the 176.4 kHz engine with 'make fw1814-experimental176' first" >&2; exit 1; \
	fi
	MACFW_INSTALL_EXPERIMENTAL176=1 bash service/install-service.sh --check-only
	$(MAKE) -C hal install
	MACFW_INSTALL_EXPERIMENTAL176=1 bash service/install-service.sh
	$(MAKE) -C control-panel install

uninstall:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: FW1814 uninstall must be run as root (use sudo make fw1814-uninstall)" >&2; \
		exit 1; \
	fi
	bash service/uninstall-service.sh
	$(MAKE) -C control-panel uninstall
	$(MAKE) -C hal uninstall

clean:
	$(MAKE) -C hal clean
	$(MAKE) -C transport clean
	$(MAKE) -C tools clean
	$(MAKE) -C control-panel clean
