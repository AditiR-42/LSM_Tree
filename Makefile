# Makefile

CC = g++
CFLAGS = -Wall -std=c++11 -O3

TARGET = lsm_tree_app
TEST_TARGET = test

# Object files for the main application
OBJECTS = lsm_tree.o main.o

# Object files for the test application
# Assuming test.cpp uses lsm_tree.o
TEST_OBJECTS = test.o lsm_tree.o

# Default target builds both
all: $(TARGET) $(TEST_TARGET)

# Link the main application
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(TARGET)

# Link the test application
$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) -o $(TEST_TARGET)

# Compile lsm_tree.cpp
lsm_tree.o: lsm_tree.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c lsm_tree.cpp -o lsm_tree.o

# Compile main.cpp
main.o: main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c main.cpp -o main.o

# Compile test.cpp
test.o: test.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c test.cpp -o test.o

# Clean up executables, object files, and generated SSTable files
clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJECTS) test.o
	rm -rf $(DATA_DIR)

# Phony targets
.PHONY: all clean