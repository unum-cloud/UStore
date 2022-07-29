
import pytest
import ukv.stl as ukv


def test_transaction_set():
    keys_count = 100
    db = ukv.DataBase()

    with ukv.Transaction(db) as txn:
        for index in range(keys_count):
            txn.set(index, str(index).encode())
    for index in range(keys_count):
        assert db.get(index) == str(index).encode()


def test_transaction_remove():
    keys_count = 100
    db = ukv.DataBase()
    for index in range(keys_count):
        db.set(index, str(index).encode())

    with ukv.Transaction(db) as txn:
        for index in range(keys_count):
            del txn[index]
    for index in range(keys_count):
        assert index not in db


def test_set_with_2_transactions():
    keys_count = 100

    db = ukv.DataBase()
    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)

    for index in range(0, keys_count, 2):
        txn1.set(index, str(index).encode())
    for index in range(1, keys_count, 2):
        txn2.set(index, str(index).encode())

    txn1.commit()
    txn2.commit()

    for index in range(keys_count):
        assert db.get(index) == str(index).encode()


def test_remove_with_2_transactions():
    keys_count = 100

    db = ukv.DataBase()
    for index in range(keys_count):
        db.set(index, str(index).encode())

    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)
    for index in range(0, keys_count, 2):
        del txn1[index]
    for index in range(1, keys_count, 2):
        del txn2[index]

    txn1.commit()
    txn2.commit()

    for index in range(keys_count):
        assert index not in db


def test_set_with_2_interleaving_transactions():
    keys_count = 100

    db = ukv.DataBase()
    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)

    for index in range(keys_count):
        txn1.set(index, 'a'.encode())
    with pytest.raises(Exception):
        for index in range(keys_count):
            txn2.set(index, 'b'.encode())

        txn2.commit()
        txn1.commit()


def test_remove_with_2_interleaving_transactions():
    keys_count = 100

    db = ukv.DataBase()
    for index in range(keys_count):
        db.set(index, str(index).encode())

    txn1 = ukv.Transaction(db)
    txn2 = ukv.Transaction(db)
    for index in range(keys_count):
        del txn1[index]
    with pytest.raises(Exception):
        for index in range(keys_count):
            del txn2[index]

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
                txn.set(col_name, key_index, value)

                # Before we commit the transaction, the key shouldn't be present in the DB
                assert key_index not in db[col_name]

                # Still, from inside the transaction, those entries must be observable
                assert key_index in txn[col_name]
                assert txn.get(col_name, key_index) == value
                assert txn[col_name][key_index] == value

    # Let's check, that those entries are globally visible
    for col_index in range(col_count):
        col_name = str(col_index)
        assert col_name in db, 'Auto-upserted collection not found!'

        for key_index in range(keys_count):
            value = str(key_index).encode()

            assert key_index in db[col_name]
            assert db.get(col_name, key_index) == value
            assert db[col_name][key_index] == value

    # Let's make sure, that updates are visible to other transactions
    with ukv.Transaction(db) as txn:
        for col_index in range(col_count):
            col_name = str(col_index)

            for key_index in range(keys_count):
                value = str(key_index).encode()

                assert key_index in txn[col_name]
                assert txn.get(col_name, key_index) == value
                assert txn[col_name][key_index] == value


def test_conflict():
    # Set with db befor get
    db1 = ukv.DataBase()
    txn = ukv.Transaction(db1)
    db1.set(0, 'value'.encode())
    with pytest.raises(Exception):
        txn.get(0)

    # Set with transaction before get
    db2 = ukv.DataBase()
    txn1 = ukv.Transaction(db2)
    with ukv.Transaction(db2) as txn2:
        txn2.set(0, 'value'.encode())
    with pytest.raises(Exception):
        txn1.get(0)


def test_transparent_reads():
    db = ukv.DataBase()
    txn = ukv.Transaction(db)
    txn.get(0)
    txn.set(1, 'value'.encode())
    db.set(0, 'value'.encode())
    with pytest.raises(Exception):
        txn.commit()
