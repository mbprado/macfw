PREFIX ?= /Library/Application Support/macfw/fw410

.PHONY: all clean install uninstall package hal tools runtime

all: hal tools

hal:
	$(MAKE) -C hal

tools:
	$(MAKE) -C tools

runtime:
	$(MAKE) -C tools transport
	$(MAKE) -C tools rateprobe
	$(MAKE) -C tools fwboot
	$(MAKE) -C tools deviceprobe

install: all
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: make install must be run as root (use sudo make install)" >&2; \
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

package: all
	chmod +x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall
	./package/build-pkg.sh

clean:
	$(MAKE) -C hal clean
	$(MAKE) -C tools clean
	rm -rf package/build package/dist
