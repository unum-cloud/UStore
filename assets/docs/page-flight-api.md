# Apache Arrow Flight API

Apache Arrow wraps a huge ecosystem of projects, just like UKV.
With standardized Arrow columnar in-memory representations any content can be exported into analytics engine of your choosing.

![UKV: Arrow](../../assets/charts/Arrow.png)

The Flight API pushes this interoperability to the next level.
Think of it as a binary analytics alternative to REST.
We implement the Flight "verbs" and "actions", so you don't even need to use our SDKs to inject or retrieve the data.

| Verb     |      Direction       | Call Examples                                                                       |
| :------- | :------------------: | :---------------------------------------------------------------------------------- |
| Put      | data in, nothing out | `write`                                                                             |
| Read     | nothing in, data out | `list_collections`                                                                  |
| Exchange |  data in, data out   | `read`, `scan`, `measure`                                                           |
| Action   |                      | `create_collection`, `remove_collection`, `begin_transaction`, `commit_transaction` |

Argument names for all the functions match those of the C standard itself.
Scalar arguments can form URI-like parameter packs, separated by `&`.
Example:

```
read?collection_name=favourites&transaction_id=abcdabcd&dont_watch
```

The payload will contain a single column table of `keys`.
