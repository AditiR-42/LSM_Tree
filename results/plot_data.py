#!/usr/bin/env python3

import matplotlib.pyplot as plt
import argparse
import sys
import os

def create_scatterplot(title="Latency of 10 RANGE Operations", xlabel="Existing Data Size (GB)", ylabel="Throughput (Queries/Sec)", name="datasize_RANGE_throughput"):
    """Creates and displays the specified scatterplot."""

    # 166666668, 333333336
    # 8333334, 83333334, 833333334

    x_data = [0.1, 1, 2, 4, 10]
    # y_data = [83333334/426.501, 83333334/433.615, 83333334/420.476, 83333334/414.389]
    y_data = [10/5.41248, 10/14.55519, 10/39.858, 10/268.862, 10/674.2]

    if not x_data or not y_data or len(x_data) != len(y_data):
        print("Error: Invalid or empty data provided for plotting.", file=sys.stderr)
        return

    plt.figure(figsize=(10, 6)) # Optional: Adjust figure size
    # plt.yscale('log')
    plt.xscale('log')
    # plt.ylim(200000, 300000)

    # Use plt.plot to draw both the line and the markers simultaneously.
    # 'ko-' is a shorthand:
    # 'k' -> black color
    # 'o' -> circle marker
    # '-' -> solid line style
    # markersize: control the size of the circles
    # linewidth: control the thickness of the line
    plt.plot(x_data, y_data, 'ko-', markersize=6, linewidth=1.5)

    # Customize plot elements (optional, but good practice)
    plt.title(title, color='black')
    plt.xlabel(xlabel, color='black')
    plt.ylabel(ylabel, color='black')

    # Optional: Customize tick colors if needed (they usually default to black/grey)
    # ax = plt.gca() # Get current axes
    # ax.tick_params(axis='x', colors='black')
    # ax.tick_params(axis='y', colors='black')

    # Add a grid for better readability (optional, customize color/style)
    plt.grid(True, color='gray', linestyle='--', linewidth=0.5, alpha=0.7)

    # Ensure layout is tight to prevent labels overlapping
    plt.tight_layout()

    # Show the plot
    plt.savefig(f"{name}.png")


create_scatterplot()