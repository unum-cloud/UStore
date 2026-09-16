"""Times a native `dict` against a persistent UStore collection over one shared key set.

Every phase runs the same work twice, once in memory and once through the database, so the printed
pairs compare directly: bulk writes, point reads, full key scans and random sampling.
The three write paths are the point of the example.
Assigning one key at a time pays the full round trip per key, assigning a whole key array hands the
engine one batch, and `broadcast` writes one shared value across a key array without materializing
a value per key.

Keys are drawn with replacement, so both stores see the same mix of inserts and overwrites, and the
native `dict` ends up smaller than the number of keys generated.

Needs `numpy` and the `ustore` bindings.
Run with `python examples/consistent_dict.py`.
"""

from random import sample
from time import perf_counter

import numpy as np
import ustore.smashtable as ustore

KEY_COUNT = 1_000_000
HIGHEST_KEY = 10_000_000
BATCH_SIZE = 10_000
VALUE = b"x"
SAMPLE_ROUNDS = 10
RANDOM_SEED = 42


def timed(function, *arguments):
    """Runs `function` and returns its elapsed seconds beside whatever it returned."""
    started = perf_counter()
    result = function(*arguments)
    return perf_counter() - started, result


def generate_keys() -> np.ndarray:
    """Draws the shared key set, with replacement, as the signed 64-bit keys the engine expects."""
    generator = np.random.default_rng(RANDOM_SEED)
    return generator.integers(low=1, high=HIGHEST_KEY, size=KEY_COUNT, dtype=np.int64)


def batch_starts(keys: np.ndarray) -> range:
    """The offsets that split `keys` into `BATCH_SIZE`-long slices."""
    return range(0, len(keys), BATCH_SIZE)


def fill_native_dict(keys: np.ndarray) -> dict:
    """Writes every key into a plain `dict`, one at a time."""
    native_dict = {}
    for key in keys:
        native_dict[key] = VALUE
    return native_dict


def fill_one_by_one(collection, keys: np.ndarray) -> None:
    """Writes every key into the collection separately, paying one round trip per key."""
    for key in keys:
        collection[key] = VALUE


def fill_in_batches(collection, keys: np.ndarray) -> None:
    """Writes the keys in `BATCH_SIZE`-long batches, passing a value for each key."""
    values = [VALUE] * BATCH_SIZE
    for start in batch_starts(keys):
        batch = keys[start : start + BATCH_SIZE]
        collection[batch] = values[: len(batch)]


def fill_by_broadcast(collection, keys: np.ndarray) -> None:
    """Writes one shared value across each batch, without building a value per key."""
    for start in batch_starts(keys):
        collection.broadcast(keys[start : start + BATCH_SIZE], VALUE)


def read_native_dict(native_dict: dict, keys: np.ndarray) -> int:
    """Reads every key from the `dict` and returns the total number of bytes seen."""
    total_bytes = 0
    for key in keys:
        total_bytes += len(native_dict[key])
    return total_bytes


def read_one_by_one(collection, keys: np.ndarray) -> int:
    """Reads every key from the collection separately and returns the total number of bytes seen."""
    total_bytes = 0
    for key in keys:
        total_bytes += len(collection[key])
    return total_bytes


def read_in_batches(collection, keys: np.ndarray) -> int:
    """Reads the keys in batches and returns how many values came back."""
    total_values = 0
    for start in batch_starts(keys):
        total_values += len(collection[keys[start : start + BATCH_SIZE]])
    return total_values


def scan_native_dict(native_dict: dict) -> int:
    """Walks every key of the `dict` and returns their sum, so the scan cannot be skipped."""
    keys_sum = 0
    for key in native_dict:
        keys_sum += int(key)
    return keys_sum


def scan_collection(collection) -> int:
    """Walks every key the collection holds and returns their sum, for comparison with the `dict`."""
    keys_sum = 0
    for key in collection.keys:
        keys_sum += int(key)
    return keys_sum


def sample_native_dict(native_dict: dict) -> None:
    """Draws `BATCH_SIZE` keys at random, which costs a full key listing every round."""
    for _ in range(SAMPLE_ROUNDS):
        sample(list(native_dict.keys()), BATCH_SIZE)


def sample_collection(collection) -> None:
    """Draws `BATCH_SIZE` keys at random, which the engine answers without listing every key."""
    for _ in range(SAMPLE_ROUNDS):
        collection.sample_keys(BATCH_SIZE)


def main() -> None:
    """Runs every phase against both stores and prints the timings side by side."""
    keys = generate_keys()
    database = ustore.DataBase()
    collection = database.main

    native_seconds, native_dict = timed(fill_native_dict, keys)
    single_seconds, _ = timed(fill_one_by_one, collection, keys)
    batch_seconds, _ = timed(fill_in_batches, collection, keys)
    broadcast_seconds, _ = timed(fill_by_broadcast, collection, keys)
    print(
        f"Writes: {native_seconds:.3f}s native, {single_seconds:.3f}s one by one, "
        f"{batch_seconds:.3f}s batched, {broadcast_seconds:.3f}s broadcast"
    )

    native_seconds, native_bytes = timed(read_native_dict, native_dict, keys)
    single_seconds, collection_bytes = timed(read_one_by_one, collection, keys)
    batch_seconds, batched_values = timed(read_in_batches, collection, keys)
    print(
        f"Reads: {native_seconds:.3f}s native, {single_seconds:.3f}s one by one, {batch_seconds:.3f}s batched, "
        f"{native_bytes} and {collection_bytes} bytes read, {batched_values} values in batches"
    )

    native_seconds, native_keys_sum = timed(scan_native_dict, native_dict)
    collection_seconds, collection_keys_sum = timed(scan_collection, collection)
    print(
        f"Scans: {native_seconds:.3f}s native, {collection_seconds:.3f}s persistent, "
        f"key sums {native_keys_sum} and {collection_keys_sum}"
    )

    native_seconds, _ = timed(sample_native_dict, native_dict)
    collection_seconds, _ = timed(sample_collection, collection)
    print(f"Samples: {native_seconds:.3f}s native, {collection_seconds:.3f}s persistent")


if __name__ == "__main__":
    main()
