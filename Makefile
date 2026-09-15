.PHONY: all all-interfaces clean \
	fw410 fw410-hal fw410-runtime fw410-gui fw410-tools fw410-install-check fw410-install fw410-uninstall fw410-clean fw410-package \
	fw1814 fw1814-hal fw1814-runtime fw1814-gui fw1814-tools fw1814-install-check fw1814-install fw1814-uninstall fw1814-clean fw1814-package \
	hal runtime gui tools all-tools install-check install uninstall package package-all

# Root targets operate on every supported interface. Device-specific targets
# remain available for focused development and individual installers.
all: all-interfaces

all-interfaces:
	$(MAKE) fw410
	$(MAKE) fw1814

fw410:
	$(MAKE) -C devices/fw410 all
fw410-hal:
	$(MAKE) -C devices/fw410 hal
fw410-runtime:
	$(MAKE) -C devices/fw410 runtime
fw410-gui:
	$(MAKE) -C devices/fw410 gui
fw410-tools:
	$(MAKE) -C devices/fw410 all-tools
fw410-install-check:
	$(MAKE) -C devices/fw410 install-check
fw410-install:
	$(MAKE) -C devices/fw410 install
fw410-uninstall:
	$(MAKE) -C devices/fw410 uninstall
fw410-clean:
	$(MAKE) -C devices/fw410 clean
fw410-package:
	$(MAKE) -C devices/fw410 clean
	$(MAKE) -C devices/fw410 all
	chmod +x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall
	./package/build-pkg.sh fw410

hal:
	$(MAKE) fw410-hal
	$(MAKE) fw1814-hal
runtime:
	$(MAKE) fw410-runtime
	$(MAKE) fw1814-runtime
gui:
	$(MAKE) fw410-gui
	$(MAKE) fw1814-gui
tools all-tools:
	$(MAKE) fw410-tools
	$(MAKE) fw1814-tools
install-check:
	$(MAKE) fw410-install-check
	$(MAKE) fw1814-install-check
install:
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "error: make install must be run as root (use sudo make install)" >&2; \
		exit 1; \
	fi
	$(MAKE) install-check
	$(MAKE) fw410-install
	$(MAKE) fw1814-install
uninstall:
	$(MAKE) fw410-uninstall
	$(MAKE) fw1814-uninstall
package: package-all

fw1814:
	$(MAKE) -C devices/fw1814 all
fw1814-hal:
	$(MAKE) -C devices/fw1814 hal
fw1814-runtime:
	$(MAKE) -C devices/fw1814 runtime
fw1814-gui:
	$(MAKE) -C devices/fw1814 gui
fw1814-tools:
	$(MAKE) -C devices/fw1814 all-tools
fw1814-install-check:
	$(MAKE) -C devices/fw1814 install-check
fw1814-install:
	$(MAKE) -C devices/fw1814 install
fw1814-uninstall:
	$(MAKE) -C devices/fw1814 uninstall
fw1814-clean:
	$(MAKE) -C devices/fw1814 clean
fw1814-package:
	$(MAKE) -C devices/fw1814 clean
	$(MAKE) -C devices/fw1814 all
	chmod +x package/build-pkg.sh package/scripts/fw1814-preinstall package/scripts/fw1814-postinstall
	./package/build-pkg.sh fw1814

package-all:
	$(MAKE) clean
	$(MAKE) all
	chmod +x package/build-all-pkg.sh package/scripts/all-preinstall package/scripts/all-postinstall \
		package/scripts/preinstall package/scripts/postinstall \
		package/scripts/fw1814-preinstall package/scripts/fw1814-postinstall
	./package/build-all-pkg.sh

clean: fw410-clean fw1814-clean
	rm -rf package/build package/dist
	chmod -x package/build-pkg.sh package/build-all-pkg.sh \
		package/scripts/all-preinstall package/scripts/all-postinstall \
		package/scripts/preinstall package/scripts/postinstall \
		package/scripts/fw1814-preinstall package/scripts/fw1814-postinstall \
		devices/fw410/service/install-service.sh devices/fw410/service/uninstall-service.sh \
		devices/fw410/tools/transport/amdtp44probe/run44.sh \
		devices/fw410/tools/transport/pcm44100playback/run44100.sh
