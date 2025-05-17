# LSM_Tree

## Overview

A write-optimized NoSQL key-value store based on an LSM-tree design. Currently, the code follows a single-threaded implementation. It will soon be updated to allow for multiple threads.

## Prerequisites

*   C++ compiler (g++-11)
*   CMake (version 3.15 or higher)
*   Make (or Ninja build tool)

## Build Instructions (without Docker)

1.  **Clone the repository:**

    ```bash
    git clone <your_repository_url>
    cd LSM_Tree
    ```

2.  **Create a build directory:**

    ```bash
    make
    ```

## Run Instructions

1.  **Run the server:**

    ```bash
    ./server
    ```

2.  **Run the client:**

    In a separate terminal:

    ```bash
    ./client
    ```

## CS265 Domain Specific Language (DSL)

The client interacts with the server using the following DSL commands:

*   `p <key> <value>`:  Inserts or updates the key-value pair.
*   `g <key>`:  Retrieves the value associated with the key.
*   `d <key>`:  Deletes the key-value pair (inserts a tombstone).
*   `r <startkey> <endkey>`: Returns the key-value pairs that fall within that range.
*   `l <filename> <threads>`: Runs a file of queries on a specified number of threads.
*   `s`: Prints statistics of the current LSM tree.


