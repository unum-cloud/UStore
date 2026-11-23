/**
 *  @file golang/shim.c
 *  @author Ash Vardanian
 *  @date November 18, 2026
 *  @brief The addresses of the Go-side trampolines, taken where C can take them, and the Arrow
 *      releases Go cannot call.
 */
#include "shim.h"

ustore_progress_callback_t ustore_go_continues(void) { return &ustoreContinues; }

static void ustore_go_admit(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count,
                            void* state) {
    ustoreAdmits((ustore_key_t*)keys, admissions, count, state);
}

ustore_predicate_callback_t ustore_go_admits(void) { return &ustore_go_admit; }

void ustore_go_release_schema(struct ArrowSchema* schema) {
    if (schema->release) schema->release(schema);
}

void ustore_go_release_array(struct ArrowDeviceArray* array) {
    if (array->array.release) array->array.release(&array->array);
}
