CFLAGS += -Wall -Wextra -Werror

workload: workload.c

run: workload
	PYTHONPATH=../../../lib:$${PYTHONPATH} python3 run.py

clean:
	$(RM) workload

.PHONY: run clean
