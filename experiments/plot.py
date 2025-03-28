import matplotlib.pyplot as plt
import numpy as np

bytes = [1, 10, 100, 1000]
time1 = [1.762, 19.585, 216.576, 2260.602]
time2 = [0.775, 10.480, 114.184, 1232.581]
# time1 = [0.270, 2.418, 23.805, 194.825]
# time2 = [0.199, 2.566, 16.125, 159.593]

plt.figure(figsize=(10, 6))
plt.plot(bytes, time1, '-o', label='Memory Capacity = 10')
plt.plot(bytes, time2, '-o', label='Memory Capacity = 50')

plt.legend(loc='upper left')

plt.xscale('log')
plt.ylim(-100, 2400)
# plt.ylim(-10, 220)
plt.xlabel('Data Size (MB)')
plt.ylabel('Response Time (sec)')
plt.title('PUT Operation Performance')
# plt.title('GET Operation Performance')

plt.grid(True, which='both', linestyle='--', linewidth=0.5, alpha=0.7)
plt.tight_layout()
# plt.savefig("unoptimized_exp_put_10.png")
# plt.savefig("unoptimized_exp_put_50.png")
# plt.savefig("unoptimized_exp_get_10.png")
# plt.savefig("unoptimized_exp_get_50.png")
plt.savefig("unoptimized_exp_put_both.png")
# plt.savefig("unoptimized_exp_get_both.png")