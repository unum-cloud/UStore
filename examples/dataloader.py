"""Feeds a UStore collection to a PyTorch `DataLoader` through a batching sampler.

`CollectionDataset` answers `__getitem__` and `__len__` over one collection, which is exactly the
map-style dataset protocol `DataLoader` expects, so the loader reads every record straight out of the
store.
`CollectionSampler` supplies the keys: sequentially by walking the collection's key stream with
`scan_keys`, or at random through the engine's own `sample`, which answers without listing every key.
A `batch_sampler` yields whole batches of keys, so it is mutually exclusive with `batch_size`,
`shuffle`, `sampler` and `drop_last` on the loader.

Needs `torch` and the `ustore` bindings.
Run with `python examples/dataloader.py`.
"""

from collections.abc import Iterator
from enum import StrEnum
from itertools import islice

import torch
from torch.utils.data import Sampler

import ustore
from ustore.keys import KEY_MAX, KEY_MIN, scan_keys

COLLECTION = "records"
KEY_COUNT = 100
BATCH_SIZE = 10
BATCHES_PER_EPOCH = 3


class Order(StrEnum):
    """How a sampler picks the keys of a batch."""

    SEQUENTIAL = "sequential"
    SAMPLED = "sampled"


class CollectionDataset:
    """One collection as a map-style dataset: a key addresses a record, and the length is its cardinality."""

    def __init__(self, database: ustore.Database, collection: int) -> None:
        self.database = database
        self.collection = collection

    def __getitem__(self, key: int) -> bytes:
        """The record one key holds, which is the tensor-free payload the loader collates."""
        value = self.database.get(key, collection=self.collection)
        if value is None:
            raise KeyError(key)
        return value

    def __len__(self) -> int:
        """How many pairs the collection holds, which the engine measures without listing them."""
        return self.database.measure(KEY_MIN, KEY_MAX, ustore.Arena(), collection=self.collection).min_cardinality


class CollectionSampler(Sampler):
    """Yields batches of keys from a UStore collection, either in key order or sampled at random."""

    def __init__(self, dataset: CollectionDataset, batch_size: int, batch_count: int, order: Order) -> None:
        """Draws `batch_count` batches of `batch_size` keys, stopping early if the collection runs out."""
        super().__init__()
        self.dataset = dataset
        self.batch_size = batch_size
        self.batch_count = batch_count
        self.order = order

    def __iter__(self) -> Iterator[list[int]]:
        """Yields one list of keys per batch, which the loader turns into one call per key."""
        if self.order is Order.SAMPLED:
            # Every draw is copied into a list before the next one rewinds the arena they share.
            arena = ustore.Arena()
            for _ in range(self.batch_count):
                drawn = self.dataset.database.sample(self.batch_size, arena, collection=self.dataset.collection)
                yield list(drawn[0])
            return

        keys = scan_keys(self.dataset.database, self.dataset.collection)
        for _ in range(self.batch_count):
            batch = list(islice(keys, self.batch_size))
            if not batch:
                return
            yield batch

    def __len__(self) -> int:
        """The number of batches this sampler yields, which is what the loader reports as its length."""
        return self.batch_count


def fill(database: ustore.Database, collection: int) -> None:
    """Writes one small value per key, so every sampled key resolves to a real record."""
    keys = list(range(KEY_COUNT))
    database.write(keys, [f"value{key}".encode() for key in keys], collection=collection)


def show_batches(dataset: CollectionDataset, order: Order) -> None:
    """Runs the loader over the collection and prints each batch the sampler produced."""
    sampler = CollectionSampler(dataset, BATCH_SIZE, BATCHES_PER_EPOCH, order)
    loader = torch.utils.data.DataLoader(dataset, batch_sampler=sampler)
    for index, batch in enumerate(loader):
        print(f"{order} batch {index}: {batch}")


def main() -> None:
    """Fills a collection and drains it through the loader twice, in key order and at random."""
    with ustore.Database.open_ram() as database:
        collection = database.collection_create(COLLECTION, modality=ustore.Modality.BLOBS)
        fill(database, collection)
        dataset = CollectionDataset(database, collection)
        print(f"The collection holds {len(dataset)} records")

        show_batches(dataset, Order.SEQUENTIAL)
        show_batches(dataset, Order.SAMPLED)


if __name__ == "__main__":
    main()
