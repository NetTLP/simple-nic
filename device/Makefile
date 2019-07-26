CC = gcc
INCLUDE := -I../../libtlp/include -I../include
LDFLAGS := -L../../libtlp/lib
LDLIBS  := -ltlp -lpthread
CFLAGS  := -g -Wall $(INCLUDE)

PROGNAME = nettlp_snic_device

all: $(PROGNAME)

.c.o:
	$(CC) $< -o $@

clean:
	rm -rf *.o
	rm -rf $(PROGNAME)
