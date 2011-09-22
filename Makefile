all: libevhttp.so benchmark

clean:
	rm -f *.o livevhttp.so benchmark

libevhttp.so: evhttp.o
	gcc -shared -o $@ $^ -lev -g $(LDFLAGS)

benchmark: benchmark.o evhttp.o
	gcc -o $@ $^ -lev -lpcre -lpthread -g $(LDFLAGS)

%.o: %.c
	gcc -c -o $@ $< -fPIC -g -O3 $(CFLAGS)
