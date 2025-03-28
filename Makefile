# Makefile

CC = g++
CFLAGS = -Wall -g -std=c++11 -I.

TARGET = lsm_tree_app
TEST_TARGET = test

OBJECTS = lsm_tree.o main.o
TEST_OBJECTS = test.o

all: $(TARGET) $(TEST_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(TARGET)

lsm_tree.o: lsm_tree.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c lsm_tree.cpp -o lsm_tree.o

main.o: main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c main.cpp -o main.o

$(TEST_TARGET): $(TEST_OBJECTS) lsm_tree.o
	$(CC) $(CFLAGS) $(TEST_OBJECTS) lsm_tree.o -o $(TEST_TARGET)

test.o: test.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c test.cpp -o test.o

clean:
	rm -f $(TARGET) $(OBJECTS) $(TEST_TARGET) $(TEST_OBJECTS)