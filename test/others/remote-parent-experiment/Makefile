.PHONY: check check-splice check-read check-mixed check-lz4 check-faults check-lifecycle clean

check: clean check-splice check-read check-mixed check-lz4 check-faults check-lifecycle

check-splice:
	PRE_DUMP_SEQUENCE=splice REMOTE_PARENT_COMPRESSION=plain ./regression.sh

check-read:
	PRE_DUMP_SEQUENCE=read REMOTE_PARENT_COMPRESSION=plain ./regression.sh

check-mixed:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=plain ./regression.sh

check-lz4:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=lz4 ./regression.sh

check-faults:
	./fault-regression.sh

check-lifecycle:
	./zdtm-smoke.sh

clean:
	rm -rf work-* fault-work-* lifecycle-work-* results
