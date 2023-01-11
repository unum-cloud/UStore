import pytest
import numpy as np

import ukv.umem as ukv


def only_explicit(col):
    col.set(3, b'x')
    col.set(4, b'y')
    assert col.has_key(3)
    assert col.has_key(4)

    assert col.get(3) == b'x'
    assert col.get(4) == b'y'
    assert col.get(4) != b'yy'

    col.pop(3)
    col.pop(4)
    assert not col.has_key(3)
    assert not col.has_key(4)


def only_explicit_batch(col):
    col.set((3, 4), (b'xx', b'yy'))
    col.set([3, 4], (b'x', b'y'))
    col.get((3, 4))
    assert col.has_key((3, 4)) == (True, True)
    vals = col.get((3, 4))
    assert len(vals) == 2
    targets = [b'x', b'y']
    for i, v in enumerate(vals):
        assert v.as_py() == targets[i]

    col.set((3, 4), None)
    assert col.has_key((3, 4)) == (False, False)

    col.pop((3, 4))


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
    col.clear()
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

    # Set same value to multiple keys
    value = 'value'.encode()
    col.broadcast(keys, value)
    for i in keys:
        assert col.get(i) == 'value'.encode()


def scan(col):
    col.clear()

    col[10] = b'a'
    col[20] = b'aa'
    col[30] = b'aaa'
    col[40] = b'aaaa'
    col[50] = b'aaaaa'
    col[60] = b'aaaaaa'

    keys = col.scan(10, 6)
    assert np.array_equal(keys, [10, 20, 30, 40, 50, 60])

    keys = col.scan(20, 5)
    assert np.array_equal(keys, [20, 30, 40, 50, 60])

    keys = col.scan(30, 1)
    assert np.array_equal(keys, [30])

    keys = col.scan(40, 2)
    assert np.array_equal(keys, [40, 50])

    keys = col.scan(60, 1)
    assert np.array_equal(keys, [60])


def iterate(col):
    col.clear()
    col[1] = b'a'
    col[2] = b'aa'
    col[3] = b'aaa'
    col[4] = b'aaaa'
    col[5] = b'aaaaa'
    col[6] = b'aaaaaa'

    # Iterate keys
    iterated_keys = []
    for key in col.keys:
        iterated_keys.append(key)

    assert iterated_keys == [1, 2, 3, 4, 5, 6]
    with pytest.raises(Exception):
        col.keys[4:2]
    iterated_keys.clear()
    for key in col.keys.since(3):
        iterated_keys.append(key)
    assert iterated_keys == [3, 4, 5, 6]
    iterated_keys.clear()
    for key in col.keys.until(4):
        iterated_keys.append(key)
    assert iterated_keys == [1, 2, 3, 4]
    assert list(col.keys) == [1, 2, 3, 4, 5, 6]

    # Iterate items
    iterated_items = []
    for item in col.items:
        iterated_items.append(item)

    assert iterated_items == [
        (1, b'a'), (2, b'aa'), (3, b'aaa'), (4, b'aaaa'), (5, b'aaaaa'), (6, b'aaaaaa')]
    iterated_items.clear()
    for item in col.items.since(3):
        iterated_items.append(item)
    assert iterated_items == [
        (3, b'aaa'), (4, b'aaaa'), (5, b'aaaaa'), (6, b'aaaaaa')]
    iterated_items.clear()
    for item in col.items.until(4):
        iterated_items.append(item)
    assert iterated_items == [
        (1, b'a'), (2, b'aa'), (3, b'aaa'), (4, b'aaaa')]
    assert list(col.items) == [
        (1, b'a'), (2, b'aa'), (3, b'aaa'), (4, b'aaaa'), (5, b'aaaaa'), (6, b'aaaaaa')]


def test_main_collection():
    db = ukv.DataBase()
    main = db.main
    only_explicit(main)
    only_explicit_batch(main)
    only_overwrite(main)
    only_operators(main)
    batch_insert(main)
    scan(main)
    iterate(main)


def test_named_collections():
    db = ukv.DataBase()
    col_sub = db['sub']
    col_dub = db['dub']
    scan(col_sub)
    scan(col_dub)
    iterate(col_sub)
    iterate(col_dub)
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
    only_explicit(txn.main)
    txn.commit()

    txn = ukv.Transaction(db)
    only_explicit_batch(txn.main)
    txn.commit()

    txn = ukv.Transaction(db)
    only_overwrite(txn.main)
    txn.commit()

    txn = ukv.Transaction(db)
    only_operators(txn.main)
    txn.commit()


def test_main_collection_txn_ctx():

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            only_explicit(txn.main)

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            only_operators(txn.main)

    with ukv.DataBase() as db:
        with ukv.Transaction(db) as txn:
            only_explicit_batch(txn.main)


def doc_iterate(col):
    keys = [1, 2, 3]
    jsons = [{'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24},
             {'Name': 'Ashot', 'Surname': 'Vardanyan', 'Age': 27},
             {'Name': 'Darvin', 'Surname': 'Harutyunyan', 'Age': 27}]

    col.set(keys, jsons)

    list_of_keys = list()
    list_of_vals = list()

    for key in col.keys:
        list_of_keys.append(key)
    assert list_of_keys == keys

    list_of_keys.clear()
    for key in col.keys.since(2):
        list_of_keys.append(key)
    assert list_of_keys == keys[1:]

    list_of_keys.clear()
    for key in col.keys.until(2):
        list_of_keys.append(key)
    assert list_of_keys == keys[:2]

    list_of_keys.clear()
    for key, value in col.items:
        list_of_keys.append(key)
        list_of_vals.append(value)
    assert list_of_keys == keys
    assert list_of_vals == jsons

    list_of_keys.clear()
    list_of_vals.clear()
    for key, value in col.items.since(2):
        list_of_keys.append(key)
        list_of_vals.append(value)
    assert list_of_keys == keys[1:]
    assert list_of_vals == jsons[1:]

    list_of_keys.clear()
    list_of_vals.clear()
    for key, value in col.items.until(2):
        list_of_keys.append(key)
        list_of_vals.append(value)
    assert list_of_keys == keys[:2]
    assert list_of_vals == jsons[:2]
    col.clear()


def doc_scan(col):
    keys = [1, 2, 3]
    jsons = [{'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24},
             {'Name': 'Ashot', 'Surname': 'Vardanyan', 'Age': 27},
             {'Name': 'Darvin', 'Surname': 'Harutyunyan', 'Age': 27}]

    col.set(keys, jsons)
    assert np.array_equal(col.scan(1, 3), [1, 2, 3])
    col.clear()


def doc_patching(col):
    json = {'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24}
    col.set(1, json)
    json_to_patch = [
        {"op": "replace", "path": "/Name", "value": "Ashot"},
        {"op": "add", "path": "/Hello", "value": "World"},
        {"op": "remove", "path": "/Age"}
    ]
    new_json = {'Name': 'Ashot', 'Surname': 'Vardanyan', 'Hello': 'World'}
    col.patch(1, json_to_patch)
    assert col.get(1) == new_json
    col.clear()


def doc_merging(col):
    json = {'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24}
    col.set(1, json)

    json_to_merge = {"Name": "Ashot", "Age": 27, 'Hello': "World"}
    new_json = {'Name': 'Ashot', 'Surname': 'Vardanyan',
                'Age': 27, 'Hello': "World"}

    col.merge(1, json_to_merge)
    assert col.get(1) == new_json
    col.clear()


def test_docs():
    db = ukv.DataBase()
    col = db['doc_col'].docs
    doc_scan(col)
    doc_iterate(col)
    doc_merging(col)
    doc_patching(col)

    json = {'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24}
    col.set(1, json)
    assert col.get(1) == json

    assert 1 in col
    del col[1]
    assert 1 not in col


def test_docs_batch():
    db = ukv.DataBase()
    doc_col = db['batch_col'].docs

    # Batch Set
    keys = [1, 2]
    jsons = [{'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24},
             {'Name': 'Ashot', 'Surname': 'Vardanyan', 'Age': 27}]
    doc_col.set(keys, jsons)
    assert doc_col.get(keys) == jsons

    # Invalid Arguments
    with pytest.raises(Exception):
        doc_col.set([1, 2, 3], jsons)
    with pytest.raises(Exception):
        doc_col.set([1], jsons)

    # Set same value to multiple keys
    json = {'Name': 'Davit', 'Surname': 'Vardanyan', 'Age': 24}
    doc_col.broadcast(keys, json)
    received_jsons = doc_col.get(keys)
    assert len(received_jsons) == 2
    assert received_jsons[0] == json
    assert received_jsons[1] == json

    # Batch Remove
    doc_col.remove(keys)
    assert 1 not in doc_col
    assert 2 not in doc_col
