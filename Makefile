.PHONY: all clean install uninstall package fw410 runtime all-tools

all: fw410

fw410:
	$(MAKE) -C fw410 all

runtime:
	$(MAKE) -C fw410 runtime

all-tools:
	$(MAKE) -C fw410 all-tools

install:
	$(MAKE) -C fw410 install

uninstall:
	$(MAKE) -C fw410 uninstall

package: runtime
	chmod +x package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall
	./package/build-pkg.sh

clean:
	$(MAKE) -C fw410 clean
	rm -rf package/build package/dist
	chmod 0755 package/build-pkg.sh package/scripts/preinstall package/scripts/postinstall \
		fw410/service/install-service.sh fw410/service/uninstall-service.sh
