.PHONY: all clean fw410 hal runtime gui tools all-tools install uninstall package \
	fw1814 fw1814-hal fw1814-runtime fw1814-tools fw1814-install \
	fw1814-uninstall fw1814-clean

all: fw410

fw410:
	$(MAKE) -C fw410 all

hal:
	$(MAKE) -C fw410 hal

runtime:
	$(MAKE) -C fw410 runtime

gui:
	$(MAKE) -C fw410 gui

tools all-tools:
	$(MAKE) -C fw410 all-tools

install:
	$(MAKE) -C fw410 install

uninstall:
	$(MAKE) -C fw410 uninstall

package:
	$(MAKE) -C fw410 clean
	$(MAKE) -C fw410 all
	chmod +x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall
	./package/build-pkg.sh

# The released default remains FW410. FW1814 is an experimental device target
# with namespaced root targets so it can be built and installed independently.
fw1814:
	$(MAKE) -C devices/fw1814 all

fw1814-hal:
	$(MAKE) -C devices/fw1814 hal

fw1814-runtime:
	$(MAKE) -C devices/fw1814 runtime

fw1814-tools:
	$(MAKE) -C devices/fw1814 tools

fw1814-install:
	$(MAKE) -C devices/fw1814 install

fw1814-uninstall:
	$(MAKE) -C devices/fw1814 uninstall

fw1814-clean:
	$(MAKE) -C devices/fw1814 clean

clean:
	$(MAKE) -C fw410 clean
	$(MAKE) -C devices/fw1814 clean
	rm -rf package/build package/dist
	chmod -x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall \
		fw410/service/install-service.sh fw410/service/uninstall-service.sh fw410/tools/transport/amdtp44probe/run44.sh \
		fw410/tools/transport/pcm44100playback/run44100.sh
