regression:
	bash ./regression.sh

shared-regression:
	python3 ./shared_regression.py

read-fault-regression:
	python3 ./read_fault_regression.py

check:
	$(MAKE) PRE_DUMP_MODE=splice regression
	$(MAKE) PRE_DUMP_MODE=read regression
	$(MAKE) shared-regression
	$(MAKE) read-fault-regression

.PHONY: regression shared-regression read-fault-regression check
