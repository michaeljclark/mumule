#!/bin/sh

# run test_mumule in a loop. test_mumule performs an atomic add
# operation in a loop and it will abort if the counter doesn't
# match. we simply add to a counter when we see "test-complete"

limit=1000
i=0
count=0

while [ ${i} -lt ${limit} ]; do
	./build/test_mumule -v 2>&1 | grep test-complete  >/dev/null
	if [ $? -eq 0 ]; then count=$(expr $count + 1); fi
	i=$(expr $i + 1)
done

if [ ${count} -eq ${limit} ]; then
	echo passed
else
	echo failed
fi