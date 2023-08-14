#! /bin/bash

set -e

rm -rf `pwd`/build/*
cd `pwd`/build &&
    cmake .. &&
    make

cd ..
cp -r `pwd`/src/include `pwd`/lib

cd `pwd`/lib/include
# 把头文件拷贝到 /usr/include/mprpc so库拷贝到/usr/lib  PATH
if [ ! -d /usr/include/mprpc ]; then
    mkdir /usr/include/mprpc
fi

for header in `ls *.h`
do
    cp $header /usr/include/mprpc
done

cd ..

cp `pwd`/libmprpc.so /usr/lib

ldconfig