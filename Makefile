.PHONY: all all-interfaces clean fw410 fw410-hal fw410-runtime fw410-gui \
	fw410-tools fw410-install fw410-uninstall fw410-clean \
	hal runtime gui tools all-tools install uninstall package \
	fw1814 fw1814-hal fw1814-runtime fw1814-gui fw1814-tools fw1814-install \
	fw1814-uninstall fw1814-clean

all: fw410

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
fw410-install:
	$(MAKE) -C devices/fw410 install
fw410-uninstall:
	$(MAKE) -C devices/fw410 uninstall
fw410-clean:
	$(MAKE) -C devices/fw410 clean

hal: fw410-hal
runtime: fw410-runtime
gui: fw410-gui
tools all-tools: fw410-tools
install: fw410-install
uninstall: fw410-uninstall

package:
	$(MAKE) -C devices/fw410 clean
	$(MAKE) -C devices/fw410 all
	chmod +x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall
	./package/build-pkg.sh

fw1814:
	$(MAKE) -C devices/fw1814 all
fw1814-hal:
	$(MAKE) -C devices/fw1814 hal
fw1814-runtime:
	$(MAKE) -C devices/fw1814 runtime
fw1814-gui:
	$(MAKE) -C devices/fw1814 gui
fw1814-tools:
	$(MAKE) -C devices/fw1814 tools
fw1814-install:
	$(MAKE) -C devices/fw1814 install
fw1814-uninstall:
	$(MAKE) -C devices/fw1814 uninstall
fw1814-clean:
	$(MAKE) -C devices/fw1814 clean

clean: fw410-clean fw1814-clean
	rm -rf package/build package/dist
	chmod -x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall \
		devices/fw410/service/install-service.sh devices/fw410/service/uninstall-service.sh \
		devices/fw410/tools/transport/amdtp44probe/run44.sh \
		devices/fw410/tools/transport/pcm44100playback/run44100.sh
