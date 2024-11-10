### Stack machine language interpreter

To test correctness:

```
make regression-full
```

To test performance:
```
make bench
```

Output on my machine:
```
make -C performance
make[1]: Entering directory '/home/metametamoon/projects/university/vm/lama-vm-make/performance'
Sort
lamac  Sort.lama
cat empty | `which time` -f "Sort\t%U" lamac -i Sort.lama
Sort    4.17
lamac -b Sort.lama
`which time` -f "Sort\t%U" ../build/vm-opt Sort.bc
Sort    1.95
make[1]: Leaving directory '/home/metametamoon/projects/university/vm/lama-vm-make/performance'
```