regression:
	bash ./regression.sh

check:
	rm -rf results
	mkdir -p results
	$(MAKE) PRE_DUMP_MODE=splice regression
	$(MAKE) PRE_DUMP_MODE=read regression

.PHONY: regression check
