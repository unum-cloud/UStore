"""
Compares the simplest usage of `dict` and `Collection`-s from UKV.
Benchmarks basic and batch consistent operations against native Python.
"""
from time import perf_counter

import ukv.umem as ukv
import numpy as np
from random import sample

keys = np.random.randint(
    low=1, high=10_000_000,
    size=1_000_000, dtype=np.int64)
value = 'x'
max_batch_size = 10_000
values = [value] * max_batch_size

# Comparing write performance on bulk imports
t1 = perf_counter()

native_dict = dict()
for key in keys:
    native_dict[key] = value

t2 = perf_counter()

db = ukv.DataBase()
acid_dict = db.main
for key in keys:
    acid_dict[key] = value

t3 = perf_counter()

for slice_start in range(0, len(keys), max_batch_size):
    acid_dict[keys[slice_start:slice_start+max_batch_size]] = values

t4 = perf_counter()

for slice_start in range(0, len(keys), max_batch_size):
    acid_dict.broadcast(keys[slice_start:slice_start+max_batch_size], value)

t5 = perf_counter()

print('Elapsed time for imports: {:.3f}s and {:.3f}s. {:.3f}s for batches {:.3f}s for broadcast'.
      format(t2-t1, t3-t2, t4-t3, t5-t4))

# Comparing read performance on random gathers
t1 = perf_counter()

for key in keys:
    native_dict[key]

t2 = perf_counter()

for key in keys:
    acid_dict[key]

t3 = perf_counter()

for slice_start in range(0, len(keys), max_batch_size):
    acid_dict[keys[slice_start:slice_start+max_batch_size]]

t4 = perf_counter()

print('Elapsed time for lookups: {:.3f}s and {:.3f}s. {:.3f}s for batches'.
      format(t2-t1, t3-t2, t4-t3))


# Comparing read performance on bulk scans
t1 = perf_counter()

keys_sum = 0
for key in native_dict.keys():
    keys_sum += key

t2 = perf_counter()

keys_sum = 0
for key in acid_dict.keys:
    keys_sum += key

t3 = perf_counter()

print('Elapsed time for scans: {:.3f}s and {:.3f}s'.format(t2-t1, t3-t2))

# Comparing random sampling
t1 = perf_counter()

for _ in range(10):
    sample(list(native_dict.keys()), max_batch_size)

t2 = perf_counter()

for _ in range(10):
    acid_dict.sample_keys(max_batch_size)

t3 = perf_counter()

print('Elapsed time for samples: {:.3f}s and {:.3f}s'.format(t2-t1, t3-t2))
