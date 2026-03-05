порядок запуска
1) make
2) dd if=/dev/urandom of=test50mb.bin bs=1M count=50
3) ./secure_copy test50mb.bin enc.bin 5
4) ./secure_copy enc.bin dec.bin 5
5) sha256sum test50mb.bin dec.bin