TESTS=$(sort $(basename $(wildcard *.lama)))

LAMAC=lamac

.PHONY: check $(TESTS)

check: $(TESTS)

$(TESTS): %: %.lama
	@echo $@
	$(LAMAC)  $<
	`which time` -f "$@\t%U" ./$@
	$(LAMAC) -b $<
	`which time` -f "$@\t%U" ../build/vm-opt $@.bc

clean:
	$(RM) test*.log *.s *~ $(TESTS) *.i
