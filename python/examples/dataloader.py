"""
    Example of collection sampling with PyTorch DataLoader
"""

import torch

from ustore.ucset import DataBase
from ustore.sampler import CollectionSampler

db = DataBase()
main = db.main
for key in range(100):
    main[key] = (f'value{key}').encode()


sampler = CollectionSampler(main, 10, 3)
dataloader = torch.utils.data.DataLoader(main, batch_sampler=sampler)

for samples in dataloader:
    print(samples)

sampler = CollectionSampler(main, 10, 3, random=True)
dataloader = torch.utils.data.DataLoader(main, batch_sampler=sampler)
for random_samples in dataloader:
    print(random_samples)
