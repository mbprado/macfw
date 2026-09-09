.PHONY: all runtime hal tools install uninstall clean

all: hal runtime

runtime:
	$(MAKE) -C transport runtime
	$(MAKE) -C tools runtime

hal:
	$(MAKE) -C hal all

tools:
	$(MAKE) -C tools all

install:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: FW1814 install must be run as root (use sudo make fw1814-install)" >&2; \
		exit 1; \
	fi
	@if [ ! -x hal/build/macfw-fw1814.driver/Contents/MacOS/macfw-fw1814 ]; then \
		echo "error: FW1814 artifacts are not built; run 'make fw1814' as a normal user first" >&2; \
		exit 1; \
	fi
	$(MAKE) -C hal install
	bash service/install-service.sh

uninstall:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: FW1814 uninstall must be run as root (use sudo make fw1814-uninstall)" >&2; \
		exit 1; \
	fi
	bash service/uninstall-service.sh
	$(MAKE) -C hal uninstall

clean:
	$(MAKE) -C hal clean
	$(MAKE) -C transport clean
	$(MAKE) -C tools clean
