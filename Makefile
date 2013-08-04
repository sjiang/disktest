CC=gcc
CFLAGS= -g -Wall -O2 -D_REENTRANT=1 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
LDFLAGS= -g

TARGET=disktest

default: $(TARGET)

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET).o -c $(TARGET).c

$(TARGET): $(TARGET).o
	$(CC) $(LDFLAGS) -o $(TARGET) $(TARGET).o -lpthread

clean:
	rm -f $(TARGET) $(TARGET).o
