all: ttt ttts

ttt: ttt.c
	gcc -g -pthread -Wall -Werror -fsanitize=address -o ttt ttt.c

ttts: ttts.c
	gcc -g -pthread -Wall -Werror -fsanitize=address -o ttts ttts.c

clean:
	rm -rf ttt
	rm -rf ttts
