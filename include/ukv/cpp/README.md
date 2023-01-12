# UKV C++ SDK

Most of UKV is implemented in C++, but the C++ implementation is isolated from the C++ interface.
That is the "Hour-Glass" pattern.
C layer provides stability and the top C++ layer brings back the syntactic sugar we all love.

## Zero Cost

The core idea of C++ is - don't pay for what you don't use.
It is both a blessing and a limitation.

```python
db.main[42] = 'Purpose of Life'.encode()
```

If you are writing something like this in Python, you don't care about performance much.
You don't care that `42` will be heap allocated, same way as the value your are assigning to that key.
If it fails, an exception will be raised and the stack will be unwound.

```cpp
database_t db;
status_t status = db.open();
blobs_collection_t main = db.main();
main[42] = "purpose of life"; // This raises, `.assign()` doesn't
```

In C++ none of that is happening.
Zero copies and no exceptions, unless you want them.

## Primary Conventions

Almost all functions return `::status_t` or an `::expected_gt` monoid.
All of them are marked `noexcept`.
Dereferencing the `expected_gt<wanted_t>` gives you the `wanted_t`.
Self-explanatory, I guess.

Assuming `_` substitutes for some randomly named `auto`, following code is valid:

```cpp
database_t db;
_ = db.open();

// Single-element access
blobs_collection_t main = db.main();
main[42] = "purpose of life";
main.at(42) = "purpose of life";
*main[42].value() == "purpose of life";
_ = main[42].clear();

// Broadcasting same value to multiple keys
main[{43, 44}] = "same value";

// Operations on smart-references
_ = main[{43, 44}].clear();
_ = main[{43, 44}].erase();
_ = main[{43, 44}].present();
_ = main[{43, 44}].length();
_ = main[{43, 44}].value();
_ = main[std::array<ukv_key_t, 3> {65, 66, 67}];
_ = main[std::vector<ukv_key_t> {65, 66, 67, 68}];
for (value_view_t value : *main[{100, 101}].value())
    (void)value;
```

> Operator calls, that need to `return` something else, will raise an exception and are explicitly marked `noexcept(false)`.

## Memory Management

Every Collection and Transaction Handle internally reuses an `arena_t`.
Spawning too many of those isn't good, but they aren't consuming any resources until the first use.
To be more explicit about where the values are exported, use the `.on(arena)` variants.

```cpp
arena_t arena(db);
_ = main[{43, 44}].on(arena).clear();
_ = main[{43, 44}].on(arena).erase();
_ = main[{43, 44}].on(arena).present();
_ = main[{43, 44}].on(arena).length();
_ = main[{43, 44}].on(arena).value();
```

## Smart References, Pointers and Iterators

The most commonly used "smart" abstractions are:

* `::strided_iterator_gt`, `::strided_range_gt`
* `::indexed_range_gt`
* `::range_gt`
* `::bits_span_t`
* `::strided_matrix_gt`

Arrays of variable-length are packed into tapes.
Those can be accessed and viewed in multiple ways:

* `::consecutive_chunks_iterator_gt`: with a base pointer and an array of `N` lengths.
* `::joined_chunks_iterator_gt`: with a base pointer and an array `N+1` offsets.
* `::embedded_chunks_iterator_gt`: with a base pointer and arrays of `N` lengths and offsets.

## Documents

By default, collections store BLOB values.
For document collections, would use a similar, but different syntax.

```cpp
docs_collection_t collection = db.main<docs_collection_t>();
collection[1] = R"( { "person": "Alice", "age": 27, "height": 1 } )";
collection[2] = R"( { "person": "Bob", "age": "27", "weight": 2 } )";
```

In addition to basic BLOB operations you also get those:

* `collection[1].patch(...)`: for JSON-Patches
* `collection[1].merge(...)`: for JSON-MergePatches
* `collection[1].gist()`: to retrieve present fields
* `collection[ckf(1, "person")]`: to lookup a specific field
* `collection[ckf(1, "/person/)]`: to lookup via a JSON-Pointer

## Tables

Just like in Python, we allow exporting document collections into a tabular form.
Those are all based on the `ukv_docs_gather()` standard function.
The C++ abstractions come in two flavors:

* Compile-time: `::table_header_gt<...>`.
* Dynamic: `::table_header_gt<std::monostate>`.

This is how one may use the compile time variant:

```cpp
auto header = table_header() //
                  .with<std::int32_t>("age")
                  .with<std::string_view>("age")
                  .with<std::string_view>("person");

auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
auto table = *maybe_table;
auto col0 = table.column<0>();
auto col1 = table.column<1>();
auto col2 = table.column<2>();
```

The dynamic analog would be:

```cpp
table_header_t header {{
    field_type_t {"age", ukv_doc_field_i32_k},
    field_type_t {"age", ukv_doc_field_str_k},
    field_type_t {"person", ukv_doc_field_str_k},
}};

auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
auto table = *maybe_table;
auto col0 = table.column(0).as<std::int32_t>();
auto col1 = table.column(1).as<std::sting_view>();
auto col2 = table.column(2).as<std::sting_view>();
```

## Graphs

Just like other interfaces, supports batch operations and can be called from inside a transaction.
Refer to `::graph_collection_t` for detailed documentation.
