ifndef CFLAGS
CFLAGS = -g3 -Wall -I./ -L./ -L/usr/lib -L/usr/local/lib/ -Wl,-rpath /usr/local/lib/ -DDAEMON
endif
ifndef LIBS
LIBS = 
endif

all: deleter

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

deleter: main.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS) 

clean:
	rm -rf *.o deleter
