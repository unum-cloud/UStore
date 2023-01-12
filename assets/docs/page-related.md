# About Related Projects

## LevelDB & RocksDB

Embedded DBMSs are plentiful.
Some are low-level, so they are expected to have low-level interfaces.
LevelDB and RocksDB are both implemented in C++.
The latter extends the interfaces of the former.
So why don't we continue adapting and developing their interfaces?

* Functions expect specific Standard Templates Library container arguments.
  * You can't bring your strings, trees, or ranges.
  * You can't pass an STL container with a custom allocator or arena.
* **Poor safety**. Almost every function call can through exceptions. It is unacceptable for a program that will always be under pressure to handle more data than fits in RAM. So expect to get a lot of `std::bad_alloc`.
* All keys are strings. [Why is it wrong?][ukv-keys-size]
* Reliance on outdated language features results in poor code quality:
  * Dynamic polymorphism: `virtual` calls.
  * Multiple inheritance.

These and other problems mean that interface can't be portable, ABI-safe, performant, or clean.

## SQL, MQL, Cypher

Numerous DBMS interfaces exist, including the infamous Structured Query Language.
Yet, we are suggesting replacing those with UKV.
So why wouldn't we adopt SQL, MQL, or Cypher?

Those interfaces imply a lot of higher-level logic that shouldn't concern the Key-Value Store.
Furthermore, using text-based protocols is error-prone and highly inefficient from a serialization and parsing standpoint.
It might be fine for OLAP requests to be called once a second, but if the function is called every microsecond, the interface must be binary.
We follow standardized community-driven RFCs rather than proprietary languages for those few places where such functionality can be implemented efficiently.
As such, for sub-document level gathers and updates, we use:


* [JSON Pointer: RFC 6901][pointer]
* [JSON Patch: RFC 6902][patch]
* [JSON MergePatch: RFC 7386][merge-patch]

## How is this different from X?

|                       | [EJDB][ejdb] | [SurrealDB][surreal] | [ArangoDB][arango] |  UKV  | Explanation                                                                                                        |
| :-------------------- | :----------: | :------------------: | :----------------: | :---: | :----------------------------------------------------------------------------------------------------------------- |
| Multi-Modal           |      ?       |          ?           |         ?          |   ✅   | Capable of storing Docs, Graphs and more in a single DBMS                                                          |
| Has a C Layer         |      ?       |          ?           |         ?          |   ✅   | Essential for compatibility                                                                                        |
| Is Fast               |      ?       |          ?           |         ?          |   ✅   |                                                                                                                    |
| High-level bindings   |      ?       |          ?           |         ?          |   ✅   | Can you call it from Python, JS, Java, GoLang or some other high-level language?                                   |
| Structured bindings   |      ?       |          ?           |         ?          |   ✅   | Are bindings capable of anything beyond passing strings to the server?                                             |
| Modular backends      |      ?       |          ?           |         ?          |   ✅   | Can you replace the DBMS without rewriting the application code?                                                   |
| Supports Transactions |      ?       |          ?           |         ?          |   ✅   | Can you compose complex consistent logic, like Read-Modify Writes or are you bound to the provided function calls? |
| Batch Operations      |      ?       |          ?           |         ?          |   ✅   | Can you read or write many entries at once? Together with Range-Scans and Sampling it is the                       |

[ejdb]: https://github.com/Softmotions/ejdb
[surreal]: https://github.com/surrealdb/surrealdb
[arango]: https://github.com/arango/arangodb
[ukv-keys-size]: https://unum.cloud/ukv/c/#integer-keys
[pointer]: https://datatracker.ietf.org/doc/html/rfc6901
[patch]: https://datatracker.ietf.org/doc/html/rfc6902
[merge-patch]: https://datatracker.ietf.org/doc/html/rfc7386
