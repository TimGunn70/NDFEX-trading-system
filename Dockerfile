# Linux dev/build environment for the trading system.

# Import ubuntu image to match CI file
FROM ubuntu:24.04

# Download necessary packages and delete package index
RUN apt-get update && apt-get install --yes --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*

# Code is mounted here at runtime
WORKDIR /src


