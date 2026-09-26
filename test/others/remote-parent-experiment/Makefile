.PHONY: check check-query check-splice check-read check-mixed check-lz4 check-faults check-lifecycle check-boundaries check-contracts check-pid-reuse check-relocation clean

check: clean check-query check-splice check-read check-mixed check-lz4 check-faults check-lifecycle check-boundaries check-contracts check-pid-reuse check-relocation

check-query:
	bash ./remote-parent-query-regression.sh

check-splice:
	PRE_DUMP_SEQUENCE=splice REMOTE_PARENT_COMPRESSION=plain bash ./query-matrix-regression.sh

check-read:
	PRE_DUMP_SEQUENCE=read REMOTE_PARENT_COMPRESSION=plain bash ./query-matrix-regression.sh

check-mixed:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=plain bash ./query-matrix-regression.sh

check-lz4:
	PRE_DUMP_SEQUENCE=mixed REMOTE_PARENT_COMPRESSION=lz4 bash ./query-matrix-regression.sh

check-faults:
	bash ./fault-regression.sh

check-lifecycle:
	bash ./zdtm-smoke.sh

check-boundaries:
	bash ./parentless-regression.sh

check-contracts:
	bash ./query-parent-contract-regression.sh

check-pid-reuse:
	bash ./pid-reuse-regression.sh

check-relocation:
	bash ./relocation-regression.sh || python3 ./accept-zero-relocation.py results/relocation/summary.json

clean:
	rm -rf work-* query-work-* query-matrix-work-* query-contract-work* fault-work-* lifecycle-work-* boundary-work-* contract-work-* pid-reuse-work-* relocation-work-* results
