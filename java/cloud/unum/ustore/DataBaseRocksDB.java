package cloud.unum.ustore;

public class DataBaseRocksDB extends DataBase {
    public static void init() {
        loadLibrary("rocksdb");
    }
}
