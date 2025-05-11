# Makefile

CC = g++
CFLAGS = -Wall -std=c++11 -O3

TARGET = lsm_tree_app
TEST_TARGET = test
BLOOM_FILTER_OBJECT = bloom_filter.o

# Object files for the main application
OBJECTS = lsm_tree.o main.o $(BLOOM_FILTER_OBJECT)

# Object files for the test application
TEST_OBJECTS = test.o lsm_tree.o $(BLOOM_FILTER_OBJECT)

# Default target builds both
all: $(TARGET) $(TEST_TARGET)

# Link the main application
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(TARGET)

# Link the test application
$(TEST_TARGET): $(TEST_OBJECTS)
	$(CC) $(CFLAGS) $(TEST_OBJECTS) -o $(TEST_TARGET)

# Compile lsm_tree.cpp
lsm_tree.o: lsm_tree.cpp lsm_tree.hh bloom_filter.hh
	$(CC) $(CFLAGS) -c lsm_tree.cpp -o lsm_tree.o

# Compile main.cpp
main.o: main.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c main.cpp -o main.o

# Compile test.cpp
test.o: test.cpp lsm_tree.hh
	$(CC) $(CFLAGS) -c test.cpp -o test.o

# Compile bloom_filter.cpp
$(BLOOM_FILTER_OBJECT): bloom_filter.cpp bloom_filter.hh
	$(CC) $(CFLAGS) -c bloom_filter.cpp -o $(BLOOM_FILTER_OBJECT)

# Clean up executables, object files, and generated SSTable files
clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJECTS) test.o $(BLOOM_FILTER_OBJECT)
	rm -rf $(DATA_DIR)

# Phony targets
.PHONY: all clean