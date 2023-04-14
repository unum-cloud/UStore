package cloud.unum.ukv;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Map; // Map abstract class
import java.lang.AutoCloseable; // Finalization
import java.util.Arrays; // Arrays.equals

/**
 * @brief An Embedded Persistent Key-Value Store with
 *        ACID Transactions and mind-boggling speed,
 *        implemented in C/C++/CUDA and wrapped into JNI.
 *
 * ## API Usage
 *
 * We provide two primary classes: `Context` and `Transaction`.
 * You start by creating a `Context` to link to the underlying DB,
 * allocate memory and perform CRUD operations on the "HEAD" state.
 * Those operations will not be consistent and may even partially fail
 * without you knowing, which part of the input was absorbed.
 *
 * To make your operations ACID, you must request a `ctx.transaction()`
 * which will encapsulate all your reads & updates. The interface of
 * the "HEAD" and transaction-based operations is identical.
 *
 * In both cases we mimic native Java `HashMap` & `Dictionary` classes.
 * Aside from that, most objects are `AutoCloseable`, to simplify the
 * resource usage and potentially auto-commit transactions on cleanup.
 *
 * Furthermore, each DB has both a "primary" collection and "named"
 * collections. To access those, pass the `String` name of the target
 * collection as the first argument to any of the following functions:
 * - containsKey(key)
 * - put(key, value)
 * - get(key)
 * - remove(key)
 * - remove(key, value)
 * - replace(key, value)
 * - putIfAbsent(key, value)
 * - getOrDefault(key, defaultValue)
 * - putAll(Map<Key, Value>)
 
 * You can expect similar behavior to native classes described here:
 * https://docs.oracle.com/javase/7/docs/api/java/util/Dictionary.html
 * https://docs.oracle.com/javase/7/docs/api/java/util/Hashtable.html
 *
 * In case of transactions you also get:
 * - rollback: To reset the state of the transaction.
 * - commit: To submit the transaction to underlying DBMS.
 *
 * ## Alternatives
 *
 * Aside from classical C/C++ implementations of RocksDB, WiredTiger,
 * LMDB and LevelDB being wrapped into JNI, there are some KVS written
 * purely in Java:
 * - https://github.com/jankotek/mapdb/
 * - https://github.com/yahoo/HaloDB
 */
public abstract class DataBase {

    /**
     * Partially implements the JDBC interface, as well as a classical
     * Java `Dictionary` API using Java Native Interface.
     * https://www.ibm.com/docs/en/informix-servers/12.10?topic=operations-handle-transactions
     */

    private static String extractLibrary(String backend) throws IOException {
        File file = File.createTempFile("libukv", ".so");
        if (file.exists()) {
            InputStream link = (DataBase.class.getResourceAsStream("/" + backend + "/libukv.so"));

            if (link != null) {
                Files.copy(
                        link,
                        file.getAbsoluteFile().toPath(),
                        StandardCopyOption.REPLACE_EXISTING);
                return file.getAbsoluteFile().toPath().toString();
            }
        }
        throw new IOException("Failed to extract library");
    }

    protected static void loadLibrary(String backend) {
        try {
            System.load(extractLibrary(backend));
        } catch (IOException e) {
            throw new RuntimeException(e.getMessage());
        }
    }

    public static class Transaction implements AutoCloseable {

        public long transactionAddress = 0;
        public long databaseAddress = 0;
        public boolean autoCommit = true;

        /**
         * When enabled, the transaction is being committed on close.
         * If the commit fails, an exception is raised.
         */
        public void setAutoCommit(boolean state) {
            autoCommit = state;
        }

        /**
         * @brief Resets the state of the transaction and creates a new "generation
         *        number" or "transaction ID" for it. Can't be used after `commit`.
         */
        public native void rollback();

        /**
         * @return true if the operation was submitted and the state of DBMS updated.
         * @brief Commits all the writes to DBMS, checking for collisions in the
         *        process. Both in writes and "non-transparent" reads. Returns
         *        operation result, but doesn't throw exceptions.
         */
        public native boolean commit();

        /**
         * Tests if the specified object is a key in this collection.
         */
        public native boolean containsKey(String collection, long key);

        public boolean containsKey(long key) {
            return containsKey(null, key);
        }

        /**
         * Maps the specified key to the specified value in this collection.
         */
        public native void put(String collection, long key, byte[] value);

        public void put(long key, byte[] value) {
            put(null, key, value);
        }

        /**
         * Returns the value to which the specified key is mapped, or null if this map
         * contains no mapping for the key.
         */
        public native byte[] get(String collection, long key);

        public byte[] get(long key) {
            return get(null, key);
        }

        /**
         * Removes the key (and its corresponding value) from this collection.
         */
        public native void erase(String collection, long key);

        public void erase(long key) {
            erase(null, key);
        }

        /**
         * Removes the key (and its corresponding value) from this collection.
         *
         * @return Previously held value.
         */
        public byte[] remove(String collection, long key) {
            byte[] value = get(collection, key);
            erase(collection, key);
            return value;
        }

        public byte[] remove(long key) {
            byte[] value = get(key);
            erase(key);
            return value;
        }

        /**
         * If the specified key is not already associated with a value (or is mapped to
         * null) associates it with the given value and returns null, else returns the
         * current value.
         */
        public void putIfAbsent(String collection, long key, byte[] value) {
            if (!containsKey(collection, key))
                put(collection, key, value);
        }

        public void putIfAbsent(long key, byte[] value) {
            if (!containsKey(key))
                put(key, value);
        }

        /**
         * Copies all of the mappings from the specified map to this collection.
         */
        public void putAll(String collection, Map<Long, byte[]> t) {
            for (Map.Entry<Long, byte[]> entry : t.entrySet())
                put(collection, entry.getKey(), entry.getValue());
        }

        public void putAll(Map<Long, byte[]> t) {
            for (Map.Entry<Long, byte[]> entry : t.entrySet())
                put(entry.getKey(), entry.getValue());
        }

        /**
         * Replaces the entry for the specified key only if it is currently mapped to
         * some value.
         */
        public byte[] replace(String collection, long key, byte[] value) {
            byte[] old = get(collection, key);
            if (!Arrays.equals(old, value))
                put(collection, key, value);
            return old;
        }

        public byte[] replace(long key, byte[] value) {
            byte[] old = get(key);
            if (!Arrays.equals(old, value))
                put(key, value);
            return old;
        }

        /**
         * Returns the value to which the specified key is mapped, or defaultValue if
         * this map contains no mapping for the key.
         */
        public byte[] getOrDefault(String collection, long key, byte[] defaultValue) {
            byte[] old = get(collection, key);
            if (old != null)
                return old;
            return defaultValue;
        }

        public byte[] getOrDefault(long key, byte[] defaultValue) {
            byte[] old = get(key);
            if (old != null)
                return old;
            return defaultValue;
        }

        /**
         * Removes the entry for the specified key only if it is currently
         * mapped to the specified value.
         *
         * @return True, iff key was found, value was equal and the removal occured.
         */
        public boolean remove(String collection, long key, byte[] value) {
            byte[] old = get(collection, key);
            if (Arrays.equals(old, value)) {
                remove(collection, key, value);
                return true;
            } else
                return false;
        }

        public boolean remove(long key, byte[] value) {
            byte[] old = get(key);
            if (Arrays.equals(old, value)) {
                remove(key, value);
                return true;
            } else
                return false;
        }

        @Override
        public void close() throws Exception {
            if (autoCommit)
                if (!commit())
                    throw new Exception("Failed to auto-commit on exit!");

        }
    }

    public static class Context extends Transaction {

        public Context() {
        }

        /**
         * @brief Initializes and opens a connection using passed config.
         *        No need to call `open` or `close` after than.
         */
        public Context(String config_json) {
            open(config_json);
        }

        public native void open(String config_json);

        public native void close_();

        /**
         * @brief Begins a new transaction with an auto-incremented identifier.
         *        By default, the transaction will be committed on `close`.
         */
        public native Transaction transaction();

        /**
         * @brief Clears the entire DB so that it contains no keys, but keeps collection
         *        names. Imposes a global lock on the entire collection, so use rarely.
         */
        public native void clear();

        /**
         * @brief Clears this collection so that it contains no keys.
         *        Imposes a global lock on the entire collection, so use rarely.
         */
        public native void clear(String collection);

        /**
         * @brief Removes a collection and all the keys in it.
         *        Imposes a global lock on the entire collection, so use rarely.
         */
        public native void remove(String collection);

        /**
         * This is the preferred Java 9+ resource management
         * approach, as finalizers have been deprecated.
         * https://stackoverflow.com/a/52889247
         * https://stackoverflow.com/a/56454348
         */
        @Override
        public void close() {
            close_();
        }

        @Override
        public void rollback() {
        }

        @Override
        public boolean commit() {
            return false;
        }
    }
}