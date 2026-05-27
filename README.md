
1) make clean && make

2) ./secure_copy -add -key "secret" -image disk.img test1.txt in/

3) ./secure_copy -list -image disk.img

4) ./secure_copy -get -image disk.img -key "secret" -out result.txt /test1.txt

