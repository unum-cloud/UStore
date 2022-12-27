# UKV Rust SDK

Rust implementation is designed to support:

* Named Collections
* ACID Transactions
* Single & Batch Operations
* [Apache DataFusion](https://arrow.apache.org/datafusion/) `TableProvider` for SQL

Using it should be, again, familiar, as it mimics [`std::collections`](https://doc.rust-lang.org/std/collections/hash_map/struct.HashMap.html):

```rust
let mut db = DataBase::new();
if db.contains_key(&42) {
    db.remove(&42);
    db.insert(43, "New Meaning".to_string());
}
for (key, value) in &db {
    println!("{key}: \"{value}\"");
}
db.clear();
```
