.PHONY: check check-splice check-read check-mixed check-lz4 check-faults check-lifecycle check-relocation clean

check: clean check-splice check-read check-mixed check-lz4 check-faults check-lifecycle check-relocation

check-splice:
	PRE_DUMP_SEQUENCE=splice REMOTE_PARENT_COMPRESSION=plain bash ./regression.sh

check-read:
	PRE_DUMP_SEQUENCE=read REMOTE_PARENT_COMPRESSION=plain bash ./regression.sh

check-mixed:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=plain bash ./regression.sh

check-lz4:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=lz4 bash ./regression.sh

check-faults:
	bash ./fault-regression.sh

check-lifecycle:
	bash ./zdtm-smoke.sh

check-relocation:
	bash ./relocation-regression.sh

clean:
	rm -rf work-* fault-work-* lifecycle-work-* relocation-work-* results
