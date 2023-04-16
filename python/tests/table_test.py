import os
import json
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.csv as csv
import pyarrow.dataset as ds
import ustore.ucset as ustore

pa.get_include()

if not os.path.exists('tmp'):
    os.makedirs('tmp')


def create_table(db):
    col = db.main
    docs = col.docs
    docs[0] = {'name': 'Lex', 'lastname': 'Fridman', 'tweets': 2221}
    docs[1] = {'name': 'Andrew', 'lastname': 'Huberman', 'tweets': 3935}
    docs[2] = {'name': 'Joe', 'lastname': 'Rogan', 'tweets': 45900}
    return col.table


def test_to_arrow():
    db = ustore.DataBase()
    table = create_table(db)
    # Tweets
    df_tweets = pd.DataFrame({'tweets': [2221, 3935, 45900]}, dtype=np.int32)
    assert table[['tweets']].astype('int32').to_arrow() \
        == pa.RecordBatch.from_pandas(df_tweets)

    # Names
    df_names = pd.DataFrame({'name': [b'Lex', b'Andrew', b'Joe']})
    assert table[['name']].astype(
        'bytes').to_arrow() == pa.RecordBatch.from_pandas(df_names)

    # Tweets and Names
    df_names_and_tweets = pd.DataFrame([
        {'name': 'Lex', 'tweets': 2221},
        {'name': 'Andrew', 'tweets': 3935},
        {'name': 'Joe', 'tweets': 45900}
    ])
    schema = pa.schema([
        pa.field('name', pa.binary()),
        pa.field('tweets', pa.int32())])

    assert table.astype({'name': 'bytes', 'tweets': 'int32'}
                        ).to_arrow() == pa.RecordBatch.from_pandas(df_names_and_tweets, schema)

    # Surnames and Names
    df_names_and_tweets = pd.DataFrame([
        {'name': 'Lex', 'lastname': 'Fridman'},
        {'name': 'Andrew', 'lastname': 'Huberman'},
        {'name': 'Joe', 'lastname': 'Rogan'}
    ])
    schema = pa.schema([
        pa.field('name', pa.binary()),
        pa.field('lastname', pa.binary())])

    assert table.astype({'name': 'bytes', 'lastname': 'bytes'}
                        ).to_arrow() == pa.RecordBatch.from_pandas(df_names_and_tweets, schema)

    # All
    all = pd.DataFrame([
        {'name': 'Lex', 'tweets': 2221, 'lastname': 'Fridman'},
        {'name': 'Andrew', 'tweets': 3935, 'lastname': 'Huberman'},
        {'name': 'Joe', 'tweets': 45900, 'lastname': 'Rogan'}
    ])
    schema = pa.schema([
        pa.field('name', pa.binary()),
        pa.field('tweets', pa.int32()),
        pa.field('lastname', pa.binary())])

    assert table.astype({'name': 'bytes', 'tweets': 'int32', 'lastname': 'bytes'}
                        ).to_arrow() == pa.RecordBatch.from_pandas(all, schema)

    db.clear()


def test_update():
    db = ustore.DataBase()
    col = db.main
    docs = col.docs
    table = col.table

    docs[0] = {'name': 'Lex', 'lastname': 'Fridman', 'tweets': 2221}
    docs[1] = {'name': 'Andrew', 'lastname': 'Huberman', 'tweets': 3935}
    docs[2] = {'name': 'Joe', 'lastname': 'Rogan', 'tweets': 45900}

    tweets = pa.array([2, 4, 5])
    names = pa.array(['Jack', 'Charls', 'Sam'])
    column_names = ['tweets', 'name']

    modifier = pa.RecordBatch.from_arrays([tweets, names], names=column_names)
    table.update(modifier)

    assert docs[0] == {'name': 'Jack', 'lastname': 'Fridman', 'tweets': 2}
    assert docs[1] == {'name': 'Charls', 'lastname': 'Huberman', 'tweets': 4}
    assert docs[2] == {'name': 'Sam', 'lastname': 'Rogan', 'tweets': 5}

    db.clear()


def test_csv():
    db = ustore.DataBase()
    table = create_table(db)
    table.astype({'name': 'str', 'tweets': 'int64'}
                 ).to_csv('tmp/pandas.csv')

    df = table.astype({'name': 'str', 'tweets': 'int64'}
                      ).to_arrow()

    exported_df = csv.read_csv('tmp/pandas.csv').to_batches()[0]
    assert df == exported_df

    db.clear()


def test_parquet():
    db = ustore.DataBase()
    table = create_table(db)
    table.astype({'name': 'str', 'tweets': 'int32'}
                 ).to_parquet('tmp/pandas.parquet')

    df = table.astype({'name': 'str', 'tweets': 'int32'}
                      ).to_arrow()

    exported_df = next(ds.dataset('tmp/pandas.parquet',
                       format='parquet').to_batches())
    assert df == exported_df

    db.clear()


def test_json():
    db = ustore.DataBase()
    table = create_table(db)
    table.astype({'name': 'str', 'tweets': 'int32'})

    expected_json = '''{"name":{"0":"Lex","1":"Andrew","2":"Joe"},"tweets":{"0":2221,"1":3935,"2":45900}}'''
    exported_json = table.to_json()
    assert exported_json == expected_json

    table.to_json('tmp/pandas.json')
    expected_json = json.loads(expected_json)
    f = open('tmp/pandas.json')
    exported_json = json.load(f)
    f.close()
    assert exported_json == expected_json

    db.clear()


def test_merge():
    db = ustore.DataBase()
    col1 = db['table1']
    docs1 = col1.docs
    table1 = col1.table
    docs1[0] = {'name': 'Lex', 'tweets': 0}
    docs1[1] = {'name': 'Andrew', 'tweets': 1}
    docs1[2] = {'name': 'Joe', 'tweets': 2}

    col2 = db['table2']
    docs2 = col2.docs
    table2 = col2.table
    docs2[0] = {'name': 'Lex', 'lastname': 'Fridman', 'tweets': 10}
    docs2[1] = {'name': 'Charls', 'lastname': 'Huberman'}
    docs2[3] = {'name': 'Carl', 'lastname': 'Rogan', 'tweets': 3}

    table1.merge(table2)

    expected = pa.RecordBatch.from_pandas(pd.DataFrame([
        {'name': 'Lex',  'tweets': 10, 'lastname': 'Fridman'},
        {'name': 'Charls', 'tweets': 1, 'lastname': 'Huberman'},
        {'name': 'Joe', 'tweets': 2, 'lastname': None},
        {'name': 'Carl', 'tweets': 3, 'lastname': 'Rogan'},
    ]))

    exported = table1.astype(
        {'name': 'str', 'tweets': 'int64', 'lastname': 'str'}).to_arrow()

    assert expected == exported


def test_sample():
    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    keys = [0, 1, 2, 3, 4, 5, 6]
    jsons = [{'name': b'Lex', 'tweets': 0},
             {'name': b'Andrew', 'tweets': 1},
             {'name': b'Joe', 'tweets': 2},
             {'name': b'Charls', 'tweets': 3},
             {'name': b'Carl', 'tweets': 4},
             {'name': b'Jack', 'tweets': 5},
             {'name': b'Fred', 'tweets': 6}]

    docs.set(keys, jsons)
    names, tweets = pa.RecordBatch.from_pylist(jsons).columns
    table.astype({'name': 'bytes', 'tweets': 'int64'})
    for _ in range(10):
        exported_names, exported_tweets = table.sample(3).to_arrow().columns
        assert set(exported_names).issubset(set(names))
        assert set(exported_tweets).issubset(set(tweets))


def test_drop():
    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    keys = [1, 2, 3]
    jsons = [{'name': b'Lex', 'lastname': b'Fridman', 'tweets': 0},
             {'name': b'Andrew', 'lastname': b'Huberman', 'tweets': 1},
             {'name': b'Joe', 'lastname': b'Rogan', 'tweets': 2}]

    docs.set(keys, jsons)
    table.astype(
        {'name': 'bytes', 'lastname': 'bytes', 'tweets': 'int64'})

    table.drop('name')
    expected_jsons = [{'name': None, 'lastname': b'Fridman', 'tweets': 0},
                      {'name': None, 'lastname': b'Huberman', 'tweets': 1},
                      {'name': None, 'lastname': b'Rogan', 'tweets': 2}]
    assert table.to_arrow().to_pylist() == expected_jsons

    docs.set(keys, jsons)
    table.drop('tweets')
    expected_jsons = [{'name': b'Lex', 'lastname': b'Fridman', 'tweets': None},
                      {'name': b'Andrew', 'lastname': b'Huberman', 'tweets': None},
                      {'name': b'Joe', 'lastname': b'Rogan', 'tweets': None}]
    assert table.to_arrow().to_pylist() == expected_jsons

    docs.set(keys, jsons)
    table.drop(['name', 'lastname'])
    expected_jsons = [{'name': None, 'lastname': None, 'tweets': 0},
                      {'name': None, 'lastname': None, 'tweets': 1},
                      {'name': None, 'lastname': None, 'tweets': 2}]
    assert table.to_arrow().to_pylist() == expected_jsons


def test_size():
    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    keys = [1, 2, 3]
    jsons = [{'name': 'Lex', 'tweets': 0},
             {'name': 'Joe', 'lastname': 'Rogan'},
             {'name': 'Andrew', 'lastname': 'Huberman', 'tweets': 1}]

    docs.set(keys, jsons)
    assert table.size == 9


def test_shape():
    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    keys = [1, 2]
    jsons = [{'name': 'Lex', 'lastname': 'Fridman', 'tweets': 0},
             {'name': 'Joe', 'lastname': 'Rogan', 'tweets': 2}]

    docs.set(keys, jsons)
    assert table.shape == (2, 3)


def test_empty():
    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    assert table.empty == True

    keys = [1, 2]
    jsons = [{'name': 'Lex', 'tweets': 0},
             {'name': 'Joe', 'tweets': 2}]

    docs.set(keys, jsons)
    assert table.empty == False


def test_insert():

    col = ustore.DataBase().main
    docs = col.docs
    table = col.table

    assert table.empty == True

    keys = [1, 2]
    jsons = [{'name': b'Lex'},
             {'name': b'Joe'}]
    docs.set(keys, jsons)

    table.insert('lastname', [b'Fridman', b'Rogan'])
    jsons = [{'name': b'Lex', 'lastname': b'Fridman'},
             {'name': b'Joe', 'lastname': b'Rogan'}]
    assert table.astype(
        {'name': 'bytes', 'lastname': 'bytes'}).to_arrow() == pa.RecordBatch.from_pylist(jsons)

    table.insert({'tweets': [0, 1], 'id': [10, 11]})
    jsons = [{'name': b'Lex', 'lastname': b'Fridman', 'tweets': 0, 'id': 10},
             {'name': b'Joe', 'lastname': b'Rogan', 'tweets': 1, 'id': 11}]

    expected = pa.RecordBatch.from_pylist(jsons)
    exported = table.astype(
        {'name': 'bytes', 'lastname': 'bytes', 'tweets': 'int64', 'id': 'int64'}).to_arrow()
    assert expected == exported


def test_from_dict():
    main = ustore.DataBase().main
    data = {'col1': [3, 2, 1, 0], 'col2': [b'a', b'b', b'c', b'd']}
    table = ustore.from_dict(main, data)
    table.astype({'col1': 'int64', 'col2': 'bytes'})
    assert pa.RecordBatch.from_pydict(data) == table.to_arrow()


def test_from_records():
    main = ustore.DataBase().main
    data = [{'col1': 3, 'col2': b'a'},
            {'col1': 2, 'col2': b'b'},
            {'col1': 1, 'col2': b'c'},
            {'col1': 0, 'col2': b'd'}]

    table = ustore.from_records(main, data)
    table.astype({'col1': 'int64', 'col2': 'bytes'})
    assert pa.RecordBatch.from_pylist(data) == table.to_arrow()


def test_rename():
    main = ustore.DataBase().main
    data = [{'col1': 3, 'col2': b'a'},
            {'col1': 2, 'col2': b'b'},
            {'col1': 1, 'col2': b'c'},
            {'col1': 0, 'col2': b'd'}]

    renamed_data = [{'col1': 3, 'col2': b'a'},
                    {'col1': 2, 'col2': b'b'},
                    {'col1': 1, 'col2': b'c'},
                    {'col1': 0, 'col2': b'd'}]

    table = ustore.from_records(main, data)

    table.rename({'col1': 'column_1', 'col2': 'column_2'})
    table.astype({'column_1': 'int64', 'column_2': 'bytes'})
    assert table.to_arrow() == pa.RecordBatch.from_pylist(renamed_data)
