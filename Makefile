TESTS=$(sort $(basename $(wildcard test*.lama)))
LOGS=$(TESTS:=.log)

LAMAC=lamac
RM=rm -rf

.PHONY: check $(TESTS)

check: $(TESTS)

build/vm: main.cpp
	g++ -m32 -g -g2 -fstack-protector-all -Werror -o build/vm $<

$(TESTS): %: %.lama build/vm
	@echo $@
	lamac -b $<
	byterun $@.bc > bytecodes-r/$@.dis
	mv $@.bc bytecodes/$@.bc
	build/vm bytecodes/$@.bc > outs/$@.out # 2> /dev/null
	diff outs/$@.out exps/$@.exp --strip-trailing-cr


clean:
	$(RM) *.s *.i *~ $(LOGS) $(TESTS)
