from multiprocessing import Process
from typing import Optional
import os
import time
import pytest

import requests as r


def one(col: Optional[str] = None, txn: Optional[int] = None):
    keys_count = 100

    params = {}
    if col:
        params['col'] = col
    if txn:
        params['txn'] = txn
    headers = {
        'Content-Type': 'application/octet-stream'
    }

    for index in range(keys_count):
        value = str(index).encode()
        post_result = r.post(
            f'http://0.0.0.0:8080/one/{index}',
            data=value,
            params=params,
            headers=headers,
        )
        assert post_result.status_code == 200 or post_result.status_code == 409, 'Failed to insert'

        doubled_value = value+value
        put_result = r.put(
            f'http://0.0.0.0:8080/one/{index}',
            data=doubled_value,
            params=params,
            headers=headers,
        )
        assert put_result.status_code == 200, 'Failed to update'

        head_result = r.head(
            f'http://0.0.0.0:8080/one/{index}',
            params=params,
            headers=headers,
        )
        assert head_result.status_code == 200, 'Failed to get'
        assert int(head_result.headers['Content-Length']) == len(
            doubled_value), 'Unexpected length'

        get_result = r.get(
            f'http://0.0.0.0:8080/one/{index}',
            params=params,
            headers=headers,
        )
        assert get_result.status_code == 200, 'Failed to get'
        assert get_result.text == doubled_value.decode(), 'Got wrong value'

    for index in range(keys_count):
        del_result = r.delete(
            f'http://0.0.0.0:8080/one/{index}',
            params=params,
            headers=headers,
        )
        assert del_result.status_code == 200, 'Failed to delete'


def test_write():
    one(None, None)
    one('main', None)


def serve():
    os.system('./build/bin/ukv_beast_server 0.0.0.0 8080 1')


def killall():
    os.system('kill $(lsof -t -i:8080)')


if __name__ == '__main__':
    killall()
    server = Process(target=serve, args=(), daemon=True)
    server.start()
    time.sleep(1)  # Sleep until the server wakes up
    test_write()
    server.kill()
    killall()
