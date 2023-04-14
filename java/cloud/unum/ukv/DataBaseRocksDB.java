package cloud.unum.ukv;

public class DataBaseRocksDB extends DataBase {
    public static void init() {
        loadLibrary("rocksdb");
    }
}
