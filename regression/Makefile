TESTS=$(sort $(basename $(wildcard test*.lama)))
LOGS=$(TESTS:=.log)

LAMAC=lamac
RM=rm -rf

.PHONY: check $(TESTS)

check: $(TESTS)

$(TESTS): %: %.lama
	@echo $@
	$(LAMAC) $@.lama -b
	byterun $@.bc > $@.dis
	cat $@.input | ../build/vm $@.bc 2> /dev/null > $@.log && diff $@.log orig/$@.log --strip-trailing-cr

expr_tests:
	make -C expressions
	make -C deep-expressions

clean:
	$(RM) *.s *.i *~ $(LOGS) $(TESTS) *.run *.sm
	make -C expressions clean
	make -C deep-expressions clean
