
import pytest
import ukv.umem as ukv


def test_transaction_set():
    keys_count = 100
    db = ukv.DataBase()

    with ukv.Transaction(db) as txn:
        for index in range(keys_count):
            txn.main.set(index, str(index).encode())
    for index in range(keys_count):
        assert db.main.get(index) == str(index).encode()


def test_transaction_remove():
    keys_count = 100
    db = ukv.DataBase()
    for index in range(keys_count):
        db.main.set(index, str(index).encode())

    with ukv.Transaction(db) as txn:
        for index in range(keys_count):
            txn.main.pop(index)
    for index in range(keys_count):
        assert index not in db.main


def test_set_with_2_transactions():
    keys_count = 100

    db = ukv.DataBase()
    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)

    for index in range(0, keys_count, 2):
        txn1.main.set(index, str(index).encode())
    for index in range(1, keys_count, 2):
        txn2.main.set(index, str(index).encode())

    txn1.commit()
    txn2.commit()

    for index in range(keys_count):
        assert db.main.get(index) == str(index).encode()


def test_remove_with_2_transactions():
    keys_count = 100

    db = ukv.DataBase()
    for index in range(keys_count):
        db.main.set(index, str(index).encode())

    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)
    for index in range(0, keys_count, 2):
        txn1.main.pop(index)
    for index in range(1, keys_count, 2):
        txn2.main.pop(index)

    txn1.commit()
    txn2.commit()

    for index in range(keys_count):
        assert index not in db.main


def test_set_with_2_interleaving_transactions():
    keys_count = 100

    db = ukv.DataBase()
    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)

    for index in range(keys_count):
        txn1.main.set(index, 'a'.encode())
    with pytest.raises(Exception):
        for index in range(keys_count):
            txn2.main.set(index, 'b'.encode())

        txn2.commit()
        txn1.commit()


def test_remove_with_2_interleaving_transactions():
    keys_count = 100

    db = ukv.DataBase()
    for index in range(keys_count):
        db.main.set(index, str(index).encode())

    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)
    for index in range(keys_count):
        txn1.main.pop(index)
    with pytest.raises(Exception):
        for index in range(keys_count):
            txn2.main.pop(index)

        txn2.commit()
        txn1.commit()


def test_transaction_with_multiple_collections():
    col_count = 100
    keys_count = 100
    db = ukv.DataBase()

    with ukv.Transaction(db) as txn:
        for col_index in range(col_count):
            col_name = str(col_index)

            for key_index in range(keys_count):
                value = str(key_index).encode()
                txn[col_name].set(key_index, value)

                # Before we commit the transaction, the key shouldn't be present in the DB
                assert key_index not in db[col_name]

                # Still, from inside the transaction, those entries must be observable
                assert key_index in txn[col_name]
                assert txn[col_name].get(key_index) == value
                assert txn[col_name][key_index] == value

    # Let's check, that those entries are globally visible
    for col_index in range(col_count):
        col_name = str(col_index)
        assert col_name in db, 'Auto-inserted collection not found!'

        for key_index in range(keys_count):
            value = str(key_index).encode()

            assert key_index in db[col_name]
            assert db[col_name].get(key_index) == value
            assert db[col_name][key_index] == value

    # Let's make sure, that updates are visible to other transactions
    with ukv.Transaction(db) as txn:
        for col_index in range(col_count):
            col_name = str(col_index)

            for key_index in range(keys_count):
                value = str(key_index).encode()

                assert key_index in txn[col_name]
                assert txn[col_name].get(key_index) == value
                assert txn[col_name][key_index] == value


def test_conflict():
    db = ukv.DataBase()
    txn1 = ukv.Transaction(db)
    txn1.main.get(0)
    with ukv.Transaction(db) as txn2:
        txn2.main.set(0, 'value'.encode())
    with pytest.raises(Exception):
        txn1.commit()


def test_dont_watch():
    db = ukv.DataBase()
    txn = ukv.Transaction(db, watch=False)
    txn.main.get(0)
    db.main.set(0, 'value'.encode())
    txn.commit()
