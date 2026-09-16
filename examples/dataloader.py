"""Feeds a UStore collection to a PyTorch `DataLoader` through a batching sampler.

A collection answers `__getitem__` and `__len__`, which is exactly the map-style dataset protocol
`DataLoader` expects, so the collection is handed to the loader directly as its dataset.
`CollectionSampler` supplies the keys: sequentially by walking the collection's key stream, or at
random through the engine's own `sample_keys`, which answers without listing every key.
A `batch_sampler` yields whole batches of keys, so it is mutually exclusive with `batch_size`,
`shuffle`, `sampler` and `drop_last` on the loader.

Needs `torch` and the `ustore` bindings.
Run with `python examples/dataloader.py`.
"""

from collections.abc import Iterator

import torch
from torch.utils.data import Sampler
from ustore.smashtable import DataBase

KEY_COUNT = 100
BATCH_SIZE = 10
BATCHES_PER_EPOCH = 3


class CollectionSampler(Sampler):
    """Yields batches of keys from a UStore collection, either in key order or sampled at random."""

    def __init__(self, collection, batch_size: int, batch_count: int, random: bool = False):
        """Draws `batch_count` batches of `batch_size` keys, stopping early if the collection runs out."""
        super().__init__()
        self.collection = collection
        self.batch_size = batch_size
        self.batch_count = batch_count
        self.random = random

    def __iter__(self) -> Iterator[list[int]]:
        """Yields one list of keys per batch, which the loader turns into one call to the dataset."""
        if self.random:
            for _ in range(self.batch_count):
                yield self.collection.sample_keys(self.batch_size).tolist()
            return

        keys = iter(self.collection.keys)
        for _ in range(self.batch_count):
            batch = list(islice_keys(keys, self.batch_size))
            if not batch:
                return
            yield batch

    def __len__(self) -> int:
        """The number of batches this sampler yields, which is what the loader reports as its length."""
        return self.batch_count


def islice_keys(keys: Iterator[int], count: int) -> Iterator[int]:
    """Takes up to `count` keys from the collection's key stream, stopping cleanly when it ends."""
    for _, key in zip(range(count), keys):
        yield key


def fill(collection) -> None:
    """Writes one small value per key, so every sampled key resolves to a real record."""
    for key in range(KEY_COUNT):
        collection[key] = f"value{key}".encode()


def show_batches(collection, random: bool) -> None:
    """Runs the loader over the collection and prints each batch the sampler produced."""
    sampler = CollectionSampler(collection, BATCH_SIZE, BATCHES_PER_EPOCH, random=random)
    loader = torch.utils.data.DataLoader(collection, batch_sampler=sampler)
    order = "random" if random else "sequential"
    for index, batch in enumerate(loader):
        print(f"{order} batch {index}: {batch}")


def main() -> None:
    """Fills a collection and drains it through the loader twice, in key order and at random."""
    database = DataBase()
    collection = database.main
    fill(collection)

    show_batches(collection, random=False)
    show_batches(collection, random=True)


if __name__ == "__main__":
    main()
