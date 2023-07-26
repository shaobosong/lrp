all:
	gcc -g -O0 -o lrpd server.c -lpthread
	gcc -g -O0 -o lrp client.c -lpthread

clean:
	rm -rf lrpd lrp
