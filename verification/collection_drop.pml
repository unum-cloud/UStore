/**
 *  @file verification/collection_drop.pml
 *  @author Ash Vardanian
 *  @date November 24, 2026
 *  @brief Spin model of the dropping mark on @c open_readers in <tt>src/nvme/database.hpp</tt>: a
 *      collection drop and the snapshot and transaction opens that refuse each other.
 *
 *  A reader counts itself with a @c fetch_add before it pins a stamp, and backs out when the old
 *  count held the mark. A drop claims the word from zero to the mark with one compare-exchange
 *  before it unlists anything, gives the runs back, and subtracts the mark, keeping the counts of
 *  readers still backing out. Neither side waits, and a refused side has moved nothing.
 *
 *  - @c no_read_of_given_back_runs: a reader that pinned a stamp listing the collection never reads
 *    its runs once the drop gave them back.
 *  - @c count_never_wraps: a reader backing out never takes the word below zero.
 *
 *  @verify pass sc
 *  @verify pass sc readers=3
 *  @verify fail sc marked=false: checks the count and gives the runs back in two steps without
 *      the mark, so a reader counting and pinning between them reads runs the drop gave back.
 *  @verify fail sc cleared_by_store=true: clears the mark with a store of zero, so a reader that
 *      found the mark and has not backed out yet takes the count below zero.
 */

#ifndef readers
#define readers 2
#endif

#ifndef marked
#define marked true
#endif

#ifndef cleared_by_store
#define cleared_by_store false
#endif

/** The high bit of the byte standing in for @c open_readers, which no count of readers reaches. */
#define dropping_k 128

/** Readers open, plus the mark while a drop runs. */
byte open_readers = 0;

/** Whether the collection is listed at the stamps readers pin from now on. */
bit listed = 1;

/** Whether the drop gave the collection's runs back to the volume. */
bit given_back = 0;

/** One snapshot or transaction open: count, back out at the mark, else pin, read and close. */
proctype reader(byte t) {
    bit no_read_of_given_back_runs;
    bit count_never_wraps;
    bit saw_mark;
    bit saw_listed;
    atomic {
        saw_mark = ((open_readers & dropping_k) != 0);
        open_readers = open_readers + 1
    }
    if
    :: saw_mark ->
        atomic {
            count_never_wraps = (open_readers != 0);
            assert(count_never_wraps);
            open_readers = open_readers - 1
        }
    :: else ->
        saw_listed = listed;
        if
        :: saw_listed ->
            no_read_of_given_back_runs = !given_back;
            assert(no_read_of_given_back_runs)
        :: else -> skip
        fi;
        atomic {
            count_never_wraps = (open_readers != 0);
            assert(count_never_wraps);
            open_readers = open_readers - 1
        }
    fi
}

/** One collection drop: claim the mark from zero, unlist, give the runs back, clear the mark. */
proctype drop(byte t) {
    bit claimed;
    if
    :: marked ->
        atomic {
            if
            :: open_readers == 0 -> open_readers = dropping_k; claimed = 1
            :: else -> claimed = 0
            fi
        }
    :: else -> claimed = (open_readers == 0)
    fi;
    if
    :: claimed ->
        listed = 0;
        given_back = 1;
        if
        :: marked && cleared_by_store -> open_readers = 0
        :: marked && !cleared_by_store -> open_readers = open_readers - dropping_k
        :: else -> skip
        fi
    :: else -> skip
    fi
}

init {
    byte index = 1;
    atomic {
        run drop(0);
        do
        :: index <= readers -> run reader(index); index++
        :: else -> break
        od
    }
}
