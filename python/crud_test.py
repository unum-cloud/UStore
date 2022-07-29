import ukv.stl as ukv
import numpy as np


def only_explicit(col):

    col.set(3, b'x')
    col.set(4, b'y')
    assert 3 in col
    assert 4 in col

    assert col.get(3) == b'x'
    assert col.get(4) == b'y'
    assert col.get(4) != b'yy'

    del col[3]
    del col[4]
    assert 3 not in col
    assert 4 not in col


def only_operators(col):

    col[1] = b'a'
    col[2] = b'bb'
    assert 1 in col
    assert 2 in col
    assert col[1] == b'a'
    assert col[2] == b'bb'

    del col[1]
    del col[2]
    assert 1 not in col
    assert 2 not in col


def only_overwrite(col):

    col.set(7, b'y')
    assert col.get(7) == b'y'
    assert col.get(7) != b'yy'

    # Overwrite with a different length string
    col.set(7, b'jjjjjjjj')
    assert col.get(7) == b'jjjjjjjj'
    col.set(7, b'yy')
    assert col.get(7) == b'yy'


def batch_insert(col):
    count_keys: int = 20
    keys: list[int] = list(range(1, count_keys + 1))
    keeper = []

    for i in keys[:count_keys//2]:
        keeper.append((f'{i}' * i).encode())

    for i in keys[count_keys//2:]:
        keeper.append((f'{i}' * int(i-count_keys//2)).encode())

    keys = np.array(keys, dtype=np.uint64)
    values = np.array(keeper)

    col.set(keys, values)

    for i in keys[:count_keys//2]:
        assert col.get(i) == (f'{i}' * i).encode()

    for i in keys[count_keys//2:]:
        assert col.get(i) == (f'{i}' * int(i-count_keys//2)).encode()


def scan(col):
    col[10] = b'a'
    col[20] = b'aa'
    col[30] = b'aaa'
    col[40] = b'aaaa'
    col[50] = b'aaaaa'
    col[60] = b'aaaaaa'

    keys, lengths = col.scan(10, 6)
    assert np.array_equal(keys, [10, 20, 30, 40, 50, 60])
    assert np.array_equal(lengths, [1, 2, 3, 4, 5, 6])

    keys, lengths = col.scan(20, 5)
    assert np.array_equal(keys, [20, 30, 40, 50, 60])
    assert np.array_equal(lengths, [2, 3, 4, 5, 6])

    keys, lengths = col.scan(30, 1)
    assert np.array_equal(keys, [30])
    assert np.array_equal(lengths, [3])

    keys, lengths = col.scan(40, 2)
    assert np.array_equal(keys, [40, 50])
    assert np.array_equal(lengths, [4, 5])

    keys, lengths = col.scan(60, 1)
    assert np.array_equal(keys, [60])
    assert np.array_equal(lengths, [6])


def test_main_collection():
    db = ukv.DataBase()
    scan(db)
    only_explicit(db)
    only_overwrite(db)
    only_operators(db)
    batch_insert(db)


def test_named_collections():
    db = ukv.DataBase()
    col_sub = db['sub']
    col_dub = db['dub']
    scan(col_sub)
    scan(col_dub)
    only_explicit(col_sub)
    only_explicit(col_dub)
    only_overwrite(col_sub)
    only_overwrite(col_dub)
    only_operators(col_sub)
    only_operators(col_dub)
    batch_insert(col_sub)
    batch_insert(col_dub)


def test_main_collection_txn():

    db = ukv.DataBase()

    txn = ukv.Transaction(db)
    only_explicit(txn)
    txn.commit()

    txn = ukv.Transaction(db)
    only_overwrite(txn)
    txn.commit()

    txn = ukv.Transaction(db)
    only_operators(txn)
    txn.commit()

    txn = ukv.Transaction(db)
    batch_insert(txn)
    txn.commit()


def test_main_collection_txn_ctx():

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            only_explicit(txn)

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            only_operators(txn)

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            batch_insert(txn)
