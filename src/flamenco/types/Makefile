PYTHON?=python3

.PHONY: all
all:
	@make -C ../../..

.PHONY: clean
clean:
	@make -C ../../.. clean

.PHONY: stubs
stubs: fd_types.json
	$(PYTHON) gen_stubs.py fd_types.h fd_types.c fd_type_names.c
	$(PYTHON) gen_fuzz.py fd_fuzz_types.h
