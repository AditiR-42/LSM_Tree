CC = cc
CFLAGS = -I/opt/homebrew/include   # GSL include path
LDFLAGS = -L/opt/homebrew/lib      # GSL library path
LIBS = -lgsl -lgslcblas            # GSL libraries

all: generator

generator:
	$(CC) generator.c -o generator $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm generator