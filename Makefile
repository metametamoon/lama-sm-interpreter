TESTS=$(sort $(basename $(wildcard test*.lama)))
LOGS=$(TESTS:=.log)

LAMAC=lamac
RM=rm -rf

.PHONY: check $(TESTS)

check: $(TESTS)

stuff:

# g++ build/main.o src/runtime/runtime.a -o build/vm -m32 -g2 -fstack-protector-all -no-pie

build/vm: src/main.cpp
	mkdir -p build
	g++ -m32 -g -g2 -fstack-protector-all -Werror -o build/main.o -c $<
	make -C src/runtime/ all
	gcc build/main.o src/runtime/gc.o src/runtime/runtime.o -o build/vm -m32 -g2 -fstack-protector-all


build/vm-opt: src/main.cpp
	mkdir -p build
	g++ -m32 -O3 -g2 -fstack-protector-all -Werror -o build/main.o -c $<
	make -C src/runtime/ all
	gcc build/main.o src/runtime/gc.o src/runtime/runtime.o -o build/vm-opt -m32 -g2 -fstack-protector-all



$(TESTS): %: %.lama build/vm
	@echo $@
	lamac -b $<
	# byterun $@.bc > bytecodes-r/$@.dis
	mv $@.bc bytecodes/$@.bc
	build/vm bytecodes/$@.bc > outs/$@.out # 2> /dev/null
	diff outs/$@.out exps/$@.exp --strip-trailing-cr

regression: build/vm
	$(MAKE) -C regression

benchmark: build/vm
	$(MAKE) -C performance

.PHONY: regression-full

regression-full: build/vm-opt
	$(MAKE) -C regression-full

clean:
	$(RM) *.s *.i *~ $(LOGS) $(TESTS)
