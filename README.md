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
`which time` -f "Sort\t%U" ./Sort
Sort    1.13
lamac -b Sort.lama
`which time` -f "Sort\t%U" ../build/vm-opt Sort.bc
Sort    1.85
make[1]: Leaving directory '/home/metametamoon/projects/university/vm/lama-vm-make/performance'
```