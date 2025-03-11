# Use an official Ubuntu base image with build tools
FROM ubuntu:22.04

# Set noninteractive to avoid prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Update package lists and install necessary tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    curl \
    wget \
    unzip \
    gcc-11 \
    g++-11 \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory inside the container
WORKDIR /app

# Copy the project source code into the container
COPY . .

# Remove the build directory if it exists
RUN rm -rf build

# Create a build directory
RUN mkdir build

# Configure the build using CMake (specifying the compiler)
RUN cmake -DCMAKE_CXX_COMPILER=/usr/bin/g++-11 -B build

# Build the project
RUN cmake --build build

# Create data directory (if it doesn't exist)
RUN mkdir -p data

# Expose the server port (if needed)
EXPOSE 12345

# Define the command to run the server
CMD ["./build/LSM_Tree_server"]