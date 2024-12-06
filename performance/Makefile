TESTS=$(sort $(basename $(wildcard *.lama)))

LAMAC=lamac

.PHONY: check $(TESTS)

check: $(TESTS)

$(TESTS): %: %.lama
	@echo $@
	$(LAMAC)  $<
	cat empty | `which time` -f "$@\t%U" ./$@
	$(LAMAC) -b $<
	@echo "optimized (-O3):"
	`which time` -f "$@\t%U" ../build/vm-opt $@.bc
	# @echo "unoptimized:" # -- we don't really need that
	# `which time` -f "$@\t%U" ../build/vm $@.bc

clean:
	$(RM) test*.log *.s *~ $(TESTS) *.i
