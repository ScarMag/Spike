Spike: Spike.c
	$(CC) Spike.c -o Spike -Wall -Wextra -pedantic -std=c99

run:
	./Spike

clean:
	rm Spike

