текстовые тестовыe файлы уже добавлены
порядок запуска, wsl
1) make (или make clean && make)
2) ПОСЛЕДОВАТЕЛЬНЫЙ ./secure_copy --mode=sequential test1.txt test2.txt test3.txt test4.txt test5.txt out/ 123
3) ПАРАЛЕЛЛЬНЫЙ ./secure_copy --mode=parallel test1.txt test2.txt test3.txt test4.txt test5.txt out/ 123
4) АВТОМАТИЧЕСКИЙ ./secure_copy test1.txt test2.txt test3.txt test4.txt test5.txt test6.txt test7.txt test8.txt test9.txt test10.txt out/ 123