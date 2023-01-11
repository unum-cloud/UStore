import numpy as np
import pandas as pd
import pyarrow as pa
import ukv.umem as ukv
pa.get_include()


def test_table():
    db = ukv.DataBase()
    col = db.main

    docs = col.docs
    docs[1] = {'name': 'Lex', 'lastname': 'Fridman', 'tweets': 2221}
    docs[2] = {'name': 'Andrew', 'lastname': 'Huberman', 'tweets': 3935}
    docs[3] = {'name': 'Joe', 'lastname': 'Rogan', 'tweets': 45900}

    table = col.table

    # Tweets
    df_tweets = pd.DataFrame({'tweets': [2221, 3935, 45900]}, dtype=np.int32)
    assert table[['tweets']].astype('int32').loc([1, 2, 3]).to_arrow() \
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

    # Update
    tweets = pa.array([2, 4, 5])
    names = pa.array(["Jack", "Charls", "Sam"])
    column_names = ["tweets", "name"]

    modifier = pa.RecordBatch.from_arrays([tweets, names], names=column_names)

    table.loc(slice(1, 3)).update(modifier)
    assert docs[1] == {'name': 'Jack', 'lastname': 'Fridman', 'tweets': 2}
    assert docs[2] == {'name': 'Charls', 'lastname': 'Huberman', 'tweets': 4}
    assert docs[3] == {'name': 'Sam', 'lastname': 'Rogan', 'tweets': 5}
