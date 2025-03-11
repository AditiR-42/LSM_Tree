# LSM_Tree

## Overview

A write-optimized NoSQL key-value store based on an LSM-tree design. Currently, the code follows a single-threaded implementation. It will soon be updated to allow for multiple threads.

## Prerequisites

*   C++ compiler (g++-11)
*   CMake (version 3.15 or higher)
*   Make (or Ninja build tool)

## Docker Instructions

1.  **Clone the repository:**

    ```bash
    git clone <your_repository_url>
    cd LSM_Tree
    ```

2.  **Build the Dockerfile:**

    ```
    docker build -t lsm_tree .
    ```

3.  **Run the Dockerfile:**

    ```
    docker run -p 12345:12345 lsm_tree
    ```

## Build Instructions (without Docker)

1.  **Clone the repository:**

    ```bash
    git clone <your_repository_url>
    cd LSM_Tree
    ```

2.  **Create a build directory:**

    ```bash
    mkdir build
    cd build
    ```

3.  **Configure the build using CMake:**

    ```bash
    cmake -DCMAKE_CXX_COMPILER=/usr/local/opt/gcc@11/bin/g++-11 ..
    ```

    If issues arise, ensure that g++-11 is installed (`ls -l /usr/bin/g++-11`). If it is not installed, install it and update the path of `-DCMAKE_CXX_COMPILER` to point to the newly installed g++-11.

4.  **Build the project:**

    ```bash
    make
    ```

## Run Instructions

1.  **Run the server:**

    ```bash
    ./bin/LSM_Tree_server
    ```

    *   **Optional:**  You can specify a different data directory:

        ```bash
        ./bin/LSM_Tree_server --data_dir=/path/to/my/data
        ```

2.  **Run the client:**

    In a separate terminal:

    ```bash
    ./bin/LSM_Tree_client
    ```

    *   The client will connect to the server at `localhost:12345` (default).  You can specify a different host and port using command-line arguments:

        ```bash
        ./bin/LSM_Tree_client --host=192.168.1.10 --port=8080
        ```

## CS265 Domain Specific Language (DSL)

The client interacts with the server using the following DSL commands:

*   `PUT <key> <value>`:  Inserts or updates the key-value pair.
*   `GET <key>`:  Retrieves the value associated with the key.
*   `DELETE <key>`:  Deletes the key-value pair (inserts a tombstone).

**Example Usage:**

TBD