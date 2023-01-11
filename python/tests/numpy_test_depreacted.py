import ukv.umem as ukv
import numpy as np


def lower_triangular(col):

    count_keys: int = 10
    keys: list[int] = list(range(1, count_keys + 1))
    pad: int = 50

    for key in keys:
        xs = ('x' * key).encode()
        col.set(key, xs)

    for key in keys:
        assert col.get(key).decode() == ('x' * key)

    values = np.zeros((count_keys, count_keys), dtype=np.uint8)
    values_lens = np.zeros(count_keys, dtype=np.uint32)
    keys = np.array(keys, dtype=np.uint64)

    values, values_lens = col.get_matrix(keys, max_length=100, padding=pad)

    for i, key in enumerate(keys):
        assert values_lens[i] == key
        assert values[i, i] == ord('x')
        if i + 1 < count_keys:
            assert values[i, i+1] == pad


def test_main_collection():
    db = ukv.DataBase()
    lower_triangular(db.main)


def test_main_collection_txn():

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            lower_triangular(txn.main)
