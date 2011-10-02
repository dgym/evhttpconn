all: libevhttpconn.so benchmark

clean:
	rm -f *.o libevhttpconn.so benchmark

libevhttpconn.so: evhttpconn.o
	gcc -shared -o $@ $^ -lev -g $(LDFLAGS)

benchmark: benchmark.o evhttpconn.o
	gcc -o $@ $^ -lev -lpcre -lpthread -g $(LDFLAGS)

%.o: %.c
	gcc -c -o $@ $< -fPIC -g -O3 $(CFLAGS)
