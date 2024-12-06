TESTS=$(sort $(basename $(wildcard test*.lama)))
LOGS=$(TESTS:=.log)

LAMAC=lamac
RM=rm -rf

.PHONY: check $(TESTS)

check: $(TESTS)

stuff:

# g++ build/main.o src/runtime/runtime.a -o build/vm -m32 -g2 -fstack-protector-all -no-pie

build/vm: src/main.cpp src/bytefile.cpp
	mkdir -p build
	g++ -m32 -g -g2 -fstack-protector-all -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -o build/main.o -c $<
	g++ -m32 -g -g2 -fstack-protector-all -Wall -Wextra -Werror -o build/bytefile.o -c src/bytefile.cpp
	make -C src/runtime/ all
	g++ build/main.o src/runtime/gc.o src/runtime/runtime.o build/bytefile.o -o build/vm -m32 -g2 -fstack-protector-all


build/vm-opt: src/main.cpp src/bytefile.cpp
	mkdir -p build
	g++ -m32 -O3 -fstack-protector-all -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -o build/main.o -c $<
	g++ -m32 -O3 -fstack-protector-all -Wall -Wextra -Werror -o build/bytefile.o -c src/bytefile.cpp
	make -C src/runtime/ all
	g++ build/main.o src/runtime/gc.o src/runtime/runtime.o build/bytefile.o -o build/vm-opt -m32 -O3 -fstack-protector-all



$(TESTS): %: %.lama build/vm
	@echo $@
	lamac -b $<
	byterun $@.bc > bytecodes-r/$@.dis
	mkdir -p bytecodes
	mv $@.bc bytecodes/$@.bc
	build/vm bytecodes/$@.bc > outs/$@.out # 2> /dev/null
	diff outs/$@.out exps/$@.exp --strip-trailing-cr


benchmark: build/vm-opt build/vm
	$(MAKE) -C performance

.PHONY: regression-full

regression-full: build/vm
	$(MAKE) -C regression-full

clean:
	$(RM) *.s *.i *~ $(LOGS) $(TESTS)
