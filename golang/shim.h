/**
 *  @file golang/shim.h
 *  @author Ash Vardanian
 *  @date November 18, 2026
 *  @brief Declarations the Go binding needs past the C headers: the Go-side trampolines a progress
 *      handler and a predicate install as C callbacks, and the release of Arrow structures.
 *
 *  The definitions live in @c shim.c because a Go file carrying an @c //export directive may hold
 *  only declarations in its preamble.
 */
#ifndef UNUM_USTORE_GO_SHIM_H
#define UNUM_USTORE_GO_SHIM_H

#include "ustore/ustore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 *  @brief The callbacks Go defines and C calls, which alone cross in that direction and so alone
 *      wear Go's spelling.
 *
 *  cgo emits a C symbol named exactly as the Go function, and Go spells an unexported function in
 *  camel case, so the names are not this file's to choose. cgo also drops @c const from the
 *  pointers it declares, so the predicate reaches C through an adapter in @c shim.c.
 */
ustore_continuation_t ustoreContinues(ustore_size_t finished, ustore_size_t total, void* state);
void ustoreAdmits(ustore_key_t* keys, ustore_admission_t* admissions, ustore_size_t count, void* state);

/** Hands back the trampolines above as function pointers. cgo renders a C function as a Go
 *  wrapper rather than an address, so the pointers have to be taken on this side. */
ustore_progress_callback_t ustore_go_continues(void);
ustore_predicate_callback_t ustore_go_admits(void);

/** Hands an Arrow schema or array back to its producer through its release callback, which Go
 *  cannot call, and leaves a released one as it is. */
void ustore_go_release_schema(struct ArrowSchema* schema);
void ustore_go_release_array(struct ArrowDeviceArray* array);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_GO_SHIM_H
