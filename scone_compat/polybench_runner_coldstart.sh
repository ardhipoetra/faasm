#!/bin/bash

echo $(( $(date '+%s%N') / 1000000)) -1

ret=1

while [ $ret -eq 1 ]; do
    dt=$(( $(date '+%s%N') / 1000000))
    inv func.invoke demo c_example  > /dev/null 2>&1
    ret=$?
done

echo $dt 0
echo $(( $(date '+%s%N') / 1000000)) 1