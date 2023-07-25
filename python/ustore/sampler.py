"""
    Sampler for DataBase collection
"""

from typing import Generator

from torch.utils.data import Sampler

from ustore.ucset import Collection


class CollectionSampler(Sampler):
    """
    Custom Sampler for samplig collection keys
    """

    def __init__(self, dataset: Collection, batch_size: int, num_samples: int, random: bool = False):
        """
        Custom Sampler for samplig collection keys
        """
        self.collection = dataset
        self.batch_size = batch_size
        self.num_samples = num_samples
        self.random = random

    def __iter__(self) -> Generator[list, None, None]:
        """
        Sample keys with batch size
        """
        if self.random:
            for _ in range(self.num_samples):
                yield self.collection.sample_keys(self.batch_size).tolist()
        else:
            iterator = iter(self.collection.keys)
            for _ in range(self.num_samples):
                batch = []
                try:
                    for _ in range(self.batch_size):
                        batch.append(next(iterator))
                except StopIteration:
                    yield batch
                    break
                yield batch

    def __len__(self) -> int:
        """
        Return collection size
        """
        return len(self.collection)
