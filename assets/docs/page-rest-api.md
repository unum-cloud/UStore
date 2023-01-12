# REST API

! This page is very outdated and will be replaced soon.

## Supported Endpoints

Modifying single entries:

* `PUT /one/id?col=str&txn=int&field=str`: Upserts data.
* `POST /one/id?col=str&txn=int&field=str`: Inserts data.
* `GET /one/id?col=str&txn=int&field=str`: Retrieves data.
* `HEAD /one/id?col=str&txn=int&field=str`: Retrieves data length.
* `DELETE /one/id?col=str&txn=int&field=str`: Deletes data.

This API drastically differs from batch APIs, as we can always provide just a single collection name and a single key.
In batch APIs we can't properly pass that inside the query URI.

Modifying collections:

* `PUT /col/name`: Upserts a collection.
* `DELETE /col/name`: Drops the entire collection.
* `DELETE /col`: Clears the main collection.

Global operations:

* `DELETE /all/`: Clears the entire DB.
* `GET /all/meta?query=str`: Retrieves DB metadata.

Supporting transactions:

* `GET /txn/client`: Returns: `{id?: int, error?: str}`
* `DELETE /txn/id`: Drops the transaction and it's contents.
* `POST /txn/id`: Commits and drops the transaction.

## Object Structure

Every Key-Value pair can be encapsulated in a dictionary-like or JSON-object-like structure.
In it's most degenerate form it can be:

```json
{
  "_id": 42,      // Like with MongoDB, stores the identifier
  "_col": null,   // Stores NULL, or the string for named collections
  "_bin": "a6cd"  // Base64-encoded binary content of the value
}
```

When working with JSON exports, we can't properly represent binary values.
To be more efficient, we also allow BSON and Message-Pack for content exchange.

Furthermore, a document may not have `_bin`, in which case the entire body of the document (aside from `_id` and `_bin`) will be exported:

```json
{
    "_id": 42,                 // ->       example/42:
    "_col": "example",         // ->           { "name": "isaac",
    "name": "isaac",           // ->             "lastname": "newton" }
    "lastname": "newton"
}
```

The final pruned object may be converted into Message-Pack and serialized into the DB as a binary value.
On each export, the decoding will be done again for @b MIMEs:

<ul>
<li><code class="docutils literal notranslate"><span class="pre">application/json</span></code>: <a href="https://datatracker.ietf.org/doc/html/rfc4627">RFC Spec</a></li>
<li><code class="docutils literal notranslate"><span class="pre">application/msgpack</span></code>: <a href="https://datatracker.ietf.org/doc/html/rfc6838">RFC Spec</a></li>
<li><code class="docutils literal notranslate"><span class="pre">application/bson</span></code>: <a href="https://bsonspec.org/">Spec</a></li>
</ul>

## Accessing Object Fields

We support the JSON Pointer (RFC 6901) to access nested document fields via a simple string path.
On batched requests we support the optional "fields" argument, which is a list of strings like: `["/name", "/mother/name"]`.
This allows users to only sample the parts of data they are need, without overloading the network with useless transfers.

Furthermore, we support JSON Patches (RFC 6902), for inplace modifications.
So instead of using a custom proprietary protocol and query language, like in MongoDB, one can perform standardized queries.

## Batched Operations

Working with @b batched data in @b AOS:

* `PUT /aos/`
  * Receives: `{objs:[obj], txn?: int, collections?: [str]|str, keys?: [int]}`.
  * Returns: `{error?: str}`.
  * If `keys` aren't given, they are being sampled as `[x['_id'] for x in objs]`.
  * If `collections` aren't given, they are being sampled as `[x['_col'] for x in objs]`.

* `PATCH /aos/`
  * Receives: `{collections?: [str]|str, keys?: [int], patch: obj, txn?: int}`.
  * Returns: `{error?: str}`.
  * If `keys` aren't given, the whole collection(s) is being patched.
  * If `collections` are also skipped, the entire DB is patched.

* `GET /aos/`
  * Receives: `{collections?: [str]|str, keys?: [int], fields?: [str], txn?: int}`.
  * Returns: `{objs?: [obj], error?: str}`.
  * If `keys` aren't given, the whole collection(s) is being retrieved.
  * If `collections` are also skipped, the entire DB is retrieved.

* `DELETE /aos/`
  * Receives: `{collections?: [str]|str, keys?: [int], fields?: [str], txn?: int}`.
  * Returns: `{error?: str}`.

* `HEAD /aos/`:
  * Receives: `{collections?: [str]|str, keys?: [int], fields?: [str], txn?: int}`.
  * Returns: `{len?: int, error?: str}`.

The optional payload members define how to parse the payload:

* `col`: Means we should put all into one collection, disregarding the `_col` fields.
* `txn`: Means we should do the operation from within a specified transaction context.

## Supported HTTP Headers

Most of the HTTP headers aren't supported by this web server, as it implements a very specific set of CRUD operations.
However, the following headers are at least partially implemented:

* `Cache-Control: no-store`
     
Means, that we should avoid caching the value in the DB on any request.
[Docs](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Cache-Control).

* `If-Match: hash`

Performs conditional checks on the existing value before overwriting it.
Those can be implemented by using Boosts CRC32 hash implementations for portability.
[Docs](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/If-Match).

* `If-Unmodified-Since: <day-name>, <day> <month> <year> <hour>:<minute>:<second> GMT`
     
Performs conditional checks on operations, similar to transactions, but of preventive nature and on the scope of a single request.
[Docs](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/If-Unmodified-Since).

* `Transfer-Encoding: gzip|deflate`
     
Describes, how the payload is compressed. Is different from `Content-Encoding`, which controls the entire session.
[Docs](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Transfer-Encoding).

## Upcoming Endpoints

Working with batched data in tape-like SOA:

* `PUT /soa/`
  * Receives: `{collections?: [str], keys: [int], txn?: int, lens: [int], tape: str}`.
  * Returns: `{error?: str}`.
* `GET /soa/`
  * Receives: `{collections?: [str], keys: [int], fields?: [str], txn?: int}`.
  * Returns: `{lens?: [int], tape?: str, error?: str}`.
* `DELETE /soa/`
  * Receives: `{collections?: [str], keys: [int], fields?: [str], txn?: int}`.
  * Returns: `{error?: str}`.
* `HEAD /soa/`
  * Receives: `{col?: str, key: int, fields?: [str], txn?: int}`.
  * Returns: `{len?: int, error?: str}`.

Working with batched data in the Apache Arrow format:

* `GET /arrow/`
  * Receives: `{collections?: [str], keys: [int], fields: [str], txn?: int}`.
  * Returns: Apache Arrow buffers

The result object will have the "application/vnd.apache.arrow.stream" MIME.

---

To test the REST API, `./src/run_rest.sh` and then cURL into it:

```sh
curl -X PUT \
  -H "Accept: Application/json" \
  -H "Content-Type: application/octet-stream" \
  0.0.0.0/8080/one/42?col=sub \
  -d 'purpose of life'

curl -i \
  -H "Accept: application/octet-stream" \
  0.0.0.0/8080/one/42?col=sub
```
