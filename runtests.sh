NTESTS=$(./main -n)
echo "Running $NTESTS tests"
for (( i=0; i <= $NTESTS-1; ++i ))
do
    echo -n "test $i: "
    ./main $i
done