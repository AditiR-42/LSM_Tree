# Makefile

CC = g++
CFLAGS = -Wall -g -std=c++11  # Add -std=c++11

TARGET = lsm_tree_app
OBJECTS = lsm_tree.o main.o

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(TARGET)

lsm_tree.o: lsm_tree.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c lsm_tree.cpp -o lsm_tree.o

main.o: main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c main.cpp -o main.o

clean:
	rm -f $(TARGET) $(OBJECTS)