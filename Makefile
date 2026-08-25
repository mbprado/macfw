.PHONY: all clean install uninstall package fw410

all: fw410

fw410:
	$(MAKE) -C fw410

install:
	$(MAKE) -C fw410 install

uninstall:
	$(MAKE) -C fw410 uninstall

package:
	$(MAKE) -C fw410 package

clean:
	$(MAKE) -C fw410 clean
