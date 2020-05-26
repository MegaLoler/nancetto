nancetto: nancetto.c
	cc -Wall -Werror -Wpedantic -g -Og nancetto.c -o nancetto -lm -ljack

.PHONY: clean
.PHONY: run

run: nancetto
	./nancetto

clean:
	rm nancetto
