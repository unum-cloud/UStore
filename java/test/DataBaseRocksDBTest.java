
import java.io.File;
import java.util.Arrays;

import org.junit.Test;

import com.unum.ukv.DataBaseRocksDB;

public class DataBaseRocksDBTest {
    static {
        DataBaseRocksDB.init();
    }

    public static void deleteDirectoryFiles(String path) {
        File directory = new File(path);
        if (!directory.isDirectory())
            return;
        
        for (File f : directory.listFiles())
            f.delete();
    }

    @Test
    public void test() {
        String path = "./tmp/";
        deleteDirectoryFiles(path);
        DataBaseRocksDB.Context ctx = new DataBaseRocksDB.Context(path);

        ctx.put(42, "hey".getBytes());
        assert Arrays.equals(ctx.get(42), "hey".getBytes()) : "Received wrong value";

        DataBaseRocksDB.Transaction txn = ctx.transaction();
        txn.put("any", 42, "meaning of life".getBytes());
        assert Arrays.equals(txn.get("any", 42), "meaning of life".getBytes()) : "Wrong philosophy";
        txn.commit();
        assert Arrays.equals(ctx.get("any", 42), "meaning of life".getBytes()) : "Accepted wrong philosophy";

        ctx.close();
        System.out.println("Success!");
    }
}
