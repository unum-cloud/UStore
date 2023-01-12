---
marp: true
---

# C++ Bindings 101

**[github.com/unum-cloud/ukv](https://github.com/unum-cloud/ukv)**

By Ashot Vardanian
Founder @ Unum.cloud

---

## Guess the Language

```python
some_value = some_dictionary[some_key]
```

---

## Yes it wasn't C++

```c
some_value = some_dictionary[some_key];
```

---

## `unordered_map<size_t, string> d` in other languages

* C++: `auto val = d[42];`
* Python: `val = d[42]`
* GoLang: `var val, found = d[42]`
* Java: `var val = d.get(42)` 😁

---

## What is `d`?

* If the interface looks the case...
* If the machine is the same...
* Can the implementation be the same?

YES

---

## But why would we want same implementation? SPEED pt. 1

[ashvardanian/CppBenchSubstrSearch](https://github.com/ashvardanian/CppBenchSubstrSearch)
<br/>

| Benchmark                | IoT      | Laptop   | Server    |
| :----------------------- | -------- | -------- | --------- |
| Python                   | 4 MB/s   | 14 MB/s  | 11 MB/s   |
| C++: `std::string::find` | 560 MB/s | 1,2 GB/s | 1,3 GB/s  |
| C/C++: AVX2              | 4,3 Gb/s | 12 GB/s  | 12,1 GB/s |

We want to take the most **flexible language**!

---

## But why would we want same implementation? SPEED pt. 2

[unum-cloud/ParallelReductions](https://github.com/unum-cloud/ParallelReductions)
<br/>

| Benchmark         | Bandwidth |
| :---------------- | --------- |
| `std::accumulate` | 5 GB/s    |
| AVX2              | 22 GB/s   |
| OpenMP            | 80 GB/s   |
| CUDA              | 817 Gb/s  |

We want to take the **relevant dialect**!

---

## But why would we want same implementation? SPEED pt. 3

[unum-cloud/UCSB](https://github.com/unum-cloud/UCSB)
<br/>

| Brand      | 0   | A   | C   | D   | E   | X   | Y   | Z   |
| :--------- | --- | --- | --- | --- | --- | --- | --- | --- |
| WiredTiger | 🥈   | 🥉   | 🥉   | 🥈   | 🥈   | 🥉   | 🥉   | 🥈   |
| LevelDB    |     | 🥈   |     |     | 🥉   |     |     | 🥉   |
| RocksDB    | 🥉   |     | 🥈   | 🥉   |     | 🥈   | 🥈   |     |
| UnumDB     | 🥇🏅  | 🥇🏅  | 🥇   | 🥇   | 🥇🏅  | 🥇🏅  | 🥇   | 🥇   |

We want to take the **best implementation**!

---

## Glossary

* Application Programming Interface: constraints on source code.

* Application Binary Interface: constraints on object code.

---

## Can we do it in C++? Why C?

ABI issues:

* encoding function names
* 32-bit vs 64-bit addressing, pointers and `size_t`
* alignment & packing rules for aggregate types: `struct`, `union`
* little- vs big-endian
* how do you store virtual tables?
* will exceptions use `setjmp` or `longjmp` or both?

How we solve them? Avoid them ☺️
And you can do it from C++, but should you?

---

## C is Much Easier to Parse

So most languages, interpreters, compilers... can do it.
Parsing templated polymorphic C++ headers, on the other hand, is tricky.
If your header conforms to C99, it can connect to *anything*.

Often, **automatically**.

---

## What we want to reach?

Let's visualize again:

```python
db = ukv.DataBase()
col[1] = 'a'.encode()
col[2] = 'bb'.encode()

assert col[1] == 'a'.encode()
assert col[2] == 'bb'.encode()

del col[1]
del col[2]
```

---

## What we want to reach? Collections

Let's visualize again:

```python
db = ukv.DataBase()
col['main'][1] = 'a'.encode()
col['other'][2] = 'bb'.encode()

assert col['main'][1] == 'a'.encode()
assert col['other'][2] == 'bb'.encode()

del col['main'][1]
del col['other'][2]
```

---

## What we want to reach? Transactions

... across different collections<br/>

```python
db = ukv.DataBase()

with ukv.Transaction(db) as txn:

    txn['balance'][bob_id] = db['balance'][bob_id] - serious_money
    txn['balance'][alice_id] = db['balance'][alice_id] + serious_money
```

---

## What we want to reach? Backend-Invariance

```python
db = ukv.DataBase('umem')
db = ukv.DataBase('rocksdb')
db = ukv.DataBase('leveldb')
db = ukv.DataBase('leveldb://0.0.0.0:8080')
db = ukv.DataBase('unumdb://0.0.0.0:8080')

with ukv.Transaction(db) as txn:

    txn['balance'][bob_id] = db['balance'][bob_id] - serious_money
    txn['balance'][alice_id] = db['balance'][alice_id] + serious_money
```

---

## Designing a C API

... may be different

```c
typedef void* ukv_database_t;
typedef void* ukv_transaction_t;

typedef uint64_t ukv_collection_t;
typedef int64_t ukv_key_t;
typedef uint32_t ukv_length_t;
typedef uint8_t* ukv_bytes_ptr_t;
typedef uint64_t ukv_size_t;
typedef char const* ukv_error_t;
```

---

## Designing a C API: Strings, Ins & Outs

```c
/**
 * @brief Opens the underlying Key-Value Store, which can be any of:
 * > embedded persistent transactional KVS
 * > embedded in-memory transactional KVS
 * > remote persistent transactional KVS
 * > remote in-memory transactional KVS
 *
 * @param[in] config  A NULL-terminated @b JSON string with configuration specs.
 * @param[out] db     A pointer to the opened KVS, unless @p `error` is filled.
 * @param[out] error  The error message to be handled by callee.
 */
typedef struct ukv_database_init_t {
    ukv_str_view_t config = NULL;
    ukv_database_t* db;
    ukv_error_t* error;
} ukv_database_init_t;

void ukv_database_init(ukv_database_init_t* c_ptr);
```

---

## Designing a C API: SoA vs AoS

Arrays of Structures:

```cpp
struct vec3 {
    float x, y, z;
};
std::vector<vec3> points; 
```

Structure of Arrays:

```cpp
std::vector<float> x;
std::vector<float> y;
std::vector<float> z;
```

---

## Designing a C API: Strides

```c
typedef struct ukv_measure_t {

    ukv_database_t db;
    ukv_error_t* error;
    ukv_transaction_t transaction = NULL;
    ukv_arena_t* arena = NULL;
    ukv_options_t options = ukv_options_default_k;
    ukv_size_t tasks_count = 1;

    // Inputs: //...//

    // Outputs //...//
} ukv_measure_t;

void ukv_write(ukv_write_t* c_ptr);
```

---

## Wrapping: PyBind11

```cpp
#include <pybind11/pybind11.h>

int add(int i, int j) {
    return i + j;
}

PYBIND11_MODULE(example, m) {
    m.doc() = "pybind11 example plugin"; // optional module docstring
    m.def("add", &add, "A function that adds two numbers");
}
```

---

## Wrapping: PyBind11 in UKV

```cpp
PYBIND11_MODULE(ukv, m) {
    auto db = py::class_<py_db_t, std::shared_ptr<py_db_t>>(m, "DataBase");
    auto collection = py::class_<py_collection_t, std::shared_ptr<py_collection_t>>(m, "Collection");
    auto txn = py::class_<py_transaction_t, std::shared_ptr<py_transaction_t>>(m, "Transaction");
    
    db.def(py::init([](std::string const& config) { ... });
    db.def("get",
           [](py_db_t& db, std::string const& collection, ukv_key_t key) { ... },
           py::arg("collection"),
           py::arg("key"));
```

Usage:

```py
db: DataBase = DataBase()
col: Collection = db['main']
val: bytes = col[42]
```

---

### Wrapping: NanoBind

![img](https://github.com/wjakob/nanobind/raw/master/docs/images/perf.svg)

---

### Wrapping: PyBind11 + CPython

```cpp
PyObject* keys_obj;
PyObject* values_obj;
if (!PyObject_CheckBuffer(keys_obj) | !PyObject_CheckBuffer(values_obj))
    throw std::invalid_argument("All arguments must implement the buffer protocol");

// Take buffer protocol handles
// Flags can be: https://docs.python.org/3/c-api/buffer.html#readonly-format
auto output_flags = PyBUF_WRITABLE | PyBUF_ANY_CONTIGUOUS | PyBUF_STRIDED;
buffer_t keys, values;
keys.initialized = PyObject_GetBuffer(keys_obj, &keys.py, PyBUF_ANY_CONTIGUOUS) == 0;
values.initialized = PyObject_GetBuffer(values_obj, &values.py, output_flags) == 0;
```

---

### Wrapping: CPython 🥩

```cpp
void fill_tensor( //
    ...
    py::handle keys_arr,
    py::handle values_arr,
    py::handle values_lengths_arr,
    ...) { 

    PyObject* keys_obj = keys_arr.ptr();
    PyObject* values_obj = values_arr.ptr();
    ...
}
```

---

### ManyLinux

---

### Wrapping: Java

"~/ukv/java/com/unum/ukv/com_unum_ukv_DataBase_Context.c"

```java
JNIEXPORT void JNICALL Java_com_unum_ukv_DataBase_00024Context_open(JNIEnv* env_java,
                                                                    jobject db_java,
                                                                    jstring config_java) {
    ukv_database_t db_ptr_c = db_ptr(env_java, db_java);
    if (db_ptr_c) {
        forward_error(env_java, "Database is already opened. Close it's current state first!");
        return;
    }

    // Temporarily copy the contents of the passed configuration string
    jboolean config_is_copy_java = JNI_FALSE;
    char const* config_c = (*env_java)->GetStringUTFChars(env_java, config_java, &config_is_copy_java);
    if ((*env_java)->ExceptionCheck(env_java))
        return;

    ukv_error_t error_c = NULL;
    ukv_database_init_t database {
        .config = config_c,
        .db = &db_ptr_c,
        .error = &error_c,
    };
    ukv_database_init(&database);
    if (config_is_copy_java == JNI_TRUE)
        (*env_java)->ReleaseStringUTFChars(env_java, config_java, config_c);
    if (forward_error(env_java, error_c))
        return;
    jfieldID db_ptr_field = find_db_field(env_java);
    (*env_java)->SetLongField(env_java, db_java, db_ptr_field, (long int)db_ptr_c);
}
```

---

### Wrapping: Show Me JAVA!

```java

public class DataBase {
    static {
        try {
            System.loadLibrary("ukv_java");
        } catch (UnsatisfiedLinkError e) {
            System.err.println("Native code library failed to load.\n" + e);
            System.exit(1);
        }
    }

    public static class Context {
        public native void open(String config_json);
        public native void put(String collection, long key, byte[] value);
        public native byte[] get(String collection, long key);
        public native void clear();
    }
}

```

---

### GoLang starts with "import C"

```go
package ukv

/*
#cgo CFLAGS: -g -Wall -I${SRCDIR}/../include
#cgo LDFLAGS: -L${SRCDIR}/../build/lib -lukv_embedded_umem -lstdc++
#include "ukv/ukv.h"
#include <stdlib.h>
*/
import "C"

...
```
---

### GoLang continues to

```go
type DataBase struct {
	raw C.ukv_database_t
}

func forwardError(error_c C.ukv_error_t) error {
	if error_c != nil {
		error_go := C.GoString(error_c)
		C.ukv_error_free(error_c)
		return errors.New(error_go)
	}
	return nil
}

func cleanArena(db *DataBase, tape_c C.ukv_bytes_ptr_t, tape_length_c C.ukv_size_t) {
	C.ukv_arena_free(db.raw, tape_c, tape_length_c)
}

func (db *DataBase) ReConnect(config string) error {

	error_c := C.ukv_error_t(nil)
	config_c := C.CString(config)
	defer C.free(unsafe.Pointer(config_c))
    ukv_database_init_t database {
        .config = config_c,
        .db = &db.raw,
        .error = &error_c,
    };

	C.ukv_database_init(&database)
	return forwardError(error_c)
}
```

---

### This is just the beginning

* REST
* C#
* Swift Bindings
* Rust Bindings
* Arrow
* Docs
* Graphs

PS: Looking for Senior C++ Engineers

**[github.com/unum-cloud/ukv](https://github.com/unum-cloud/ukv)**
