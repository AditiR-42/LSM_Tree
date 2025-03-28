import matplotlib.pyplot as plt
import numpy as np

bytes = [1, 10, 100, 1000]
time1 = [1.762 / 1, 19.585 / 10, 216.576 / 100, 2260.602 / 1000]
time2 = [0.775 / 1, 10.480 / 10, 114.184 / 100, 1232.581 / 1000]
# time1 = [2.125, 20.403, 202.019, 1670.879]
# time2 = [1.574, 20.395, 148.469, 1367.18]

plt.figure(figsize=(10, 6))
plt.plot(bytes, time1, '-o', label='Memory Capacity = 10')
plt.plot(bytes, time2, '-o', label='Memory Capacity = 50')

plt.legend(loc='upper left')

plt.xscale('log')
# plt.ylim(-100, 2400)
# plt.ylim(-10, 220)
plt.xlabel('Data Size (MB)')
plt.ylabel('Response Time (sec)')
# plt.title('PUT Operation Performance')
plt.title('GET Operation Performance')

plt.grid(True, which='both', linestyle='--', linewidth=0.5, alpha=0.7)
plt.tight_layout()
# plt.savefig("unoptimized_exp_put_10.png")
# plt.savefig("unoptimized_exp_put_50.png")
# plt.savefig("unoptimized_exp_get_10.png")
# plt.savefig("unoptimized_exp_get_50.png")
# plt.savefig("unoptimized_exp_put_both.png")
plt.savefig("unoptimized_exp_get_both.png")