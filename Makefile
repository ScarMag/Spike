Spike: Spike.c
	$(CC) Spike.c -o Spike -Wall -Wextra -pedantic -std=c99

run: Spike
	./Spike $(ARG)

clean:
	rm Spike

