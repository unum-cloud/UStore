import java.io.File;
import java.util.Arrays;

import org.junit.Test;

import cloud.unum.ukv.DataBaseLevelDB;

public class DataBaseLevelDBTest {
    static {
        DataBaseLevelDB.init();
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
        DataBaseLevelDB.Context ctx = new DataBaseLevelDB.Context(path);

        ctx.put(42, "hey".getBytes());
        assert Arrays.equals(ctx.get(42), "hey".getBytes()) : "Received wrong value";

        ctx.close();
        System.out.println("Success!");
    }
}
