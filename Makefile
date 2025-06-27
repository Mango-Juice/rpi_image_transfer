SUBDIRS = drivers apis programs

all:
	@echo "Building all components..."
	@for dir in $(SUBDIRS); do \
		echo "Building $$dir..."; \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@echo "Cleaning all components..."
	@for dir in $(SUBDIRS); do \
		echo "Cleaning $$dir..."; \
		$(MAKE) -C $$dir clean; \
	done

drivers:
	$(MAKE) -C drivers

apis:
	$(MAKE) -C apis

programs: apis
	$(MAKE) -C programs

install: all
	@for dir in $(SUBDIRS); do \
		echo "Installing $$dir..."; \
		$(MAKE) -C $$dir install 2>/dev/null || true; \
	done

.PHONY: all clean drivers apis programs install
