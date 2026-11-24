// Apache Arrow columns viewed as inputs, and arena outputs exported as columns, mirroring arrow.h.
//
// File: golang/arrow.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import "unsafe"

/*
#cgo noescape ustore_keys_view_from_arrow
#cgo nocallback ustore_keys_view_from_arrow
#cgo noescape ustore_strings_view_from_arrow
#cgo nocallback ustore_strings_view_from_arrow
#cgo noescape ustore_keys_view_to_arrow
#cgo nocallback ustore_keys_view_to_arrow
#cgo noescape ustore_strings_view_to_arrow
#cgo nocallback ustore_strings_view_to_arrow
#cgo noescape ustore_docs_column_view_to_arrow
#cgo nocallback ustore_docs_column_view_to_arrow

#include "shim.h"
*/
import "C"

// ArrowSchema is the Arrow C data interface's schema of one column, laid out as struct ArrowSchema,
// so any Arrow library reads it through its address.
type ArrowSchema C.struct_ArrowSchema

// ArrowArray is the Arrow C data interface's array of one column, laid out as struct ArrowArray.
type ArrowArray C.struct_ArrowArray

// ArrowDeviceArray is an [ArrowArray] beside the device its buffers live on, laid out as struct
// ArrowDeviceArray.
type ArrowDeviceArray C.struct_ArrowDeviceArray

// Release hands the schema back to its producer, once; a released schema is safe to release again.
func (schema *ArrowSchema) Release() { C.ustore_go_release_schema((*C.struct_ArrowSchema)(schema)) }

// Release hands the array back to its producer, once; a released array is safe to release again.
func (array *ArrowDeviceArray) Release() {
	C.ustore_go_release_array((*C.struct_ArrowDeviceArray)(array))
}

// Array reports the column itself, which the converters read as they read a host array.
func (array *ArrowDeviceArray) Array() *ArrowArray {
	return (*ArrowArray)(unsafe.Pointer(&array.array))
}

// StringsLayout is the layout a tape of strings exports as.
type StringsLayout int32

const (
	// StringsU32Tape is Arrow's binary layout, with 32-bit offsets.
	StringsU32Tape StringsLayout = C.ustore_strings_u32_tape_k
	// StringsU64Tape is Arrow's large binary layout, with 64-bit offsets.
	StringsU64Tape StringsLayout = C.ustore_strings_u64_tape_k
)

// arrowed turns the status a converter or an exporter answers into a Go error.
func arrowed(status C.ustore_status_t) error {
	if status == C.ustore_status_success_k {
		return nil
	}
	return fail(Status(status), "the Arrow column could not be converted")
}

// KeysViewFromArrow views the 16-byte keys of a fixed_size_binary(16) column in place, tagged
// arrow.uuid or not, viewing the column's buffers.
func KeysViewFromArrow(schema *ArrowSchema, array *ArrowArray) ([]Key, error) {
	var keys C.ustore_keys_view_t
	var count C.ustore_size_t
	if err := arrowed(C.ustore_keys_view_from_arrow((*C.struct_ArrowSchema)(schema), (*C.struct_ArrowArray)(array), &keys, &count)); err != nil {
		return nil, err
	}
	return keysAt(keys.data, 0, uint64(count)), nil
}

// StringsViewFromArrow views the byte strings of a binary, utf8, large binary or large utf8 column
// in place as a [Tape], viewing the column's buffers.
func StringsViewFromArrow(schema *ArrowSchema, array *ArrowArray) (Tape, error) {
	var strings Strings
	var count C.ustore_size_t
	if err := arrowed(C.ustore_strings_view_from_arrow((*C.struct_ArrowSchema)(schema), (*C.struct_ArrowArray)(array), &strings.raw, &count)); err != nil {
		return Tape{}, err
	}
	strings.count = uint(count)
	return strings.Tape(), nil
}

// KeysViewToArrow exports keys a call wrote into arena as one fixed_size_binary(16) column tagged
// arrow.uuid, in place, living until the arena's next call or close; release both outputs after
// reading them.
func KeysViewToArrow(keys []Key, arena *Arena, schema *ArrowSchema, array *ArrowDeviceArray) error {
	view := C.ustore_keys_view_t{data: (*C.ustore_key_t)(unsafe.Pointer(unsafe.SliceData(keys))), stride: C.ustore_size_t(unsafe.Sizeof(Key{}))}
	outputs, err := arena.lower()
	if err != nil {
		return err
	}
	return arrowed(C.ustore_keys_view_to_arrow(view, C.ustore_size_t(len(keys)), outputs, (*C.struct_ArrowSchema)(schema), (*C.struct_ArrowDeviceArray)(array)))
}

// StringsViewToArrow exports strings a call wrote into arena as one binary or large binary column,
// in place when the tape already has layout, living until the arena's next call or close.
func StringsViewToArrow(strings Strings, layout StringsLayout, arena *Arena, schema *ArrowSchema, array *ArrowDeviceArray) error {
	outputs, err := arena.lower()
	if err != nil {
		return err
	}
	return arrowed(C.ustore_strings_view_to_arrow(strings.raw, C.ustore_size_t(strings.count), C.ustore_strings_layout_t(layout), outputs, (*C.struct_ArrowSchema)(schema), (*C.struct_ArrowDeviceArray)(array)))
}

// DocsColumnViewToArrow exports one column a gather of fieldType wrote into arena as an Arrow
// column whose validity buffer is the column's validities, living until the arena's next call or
// close.
func DocsColumnViewToArrow(column DocsColumn, fieldType DocsFieldType, arena *Arena, schema *ArrowSchema, array *ArrowDeviceArray) error {
	outputs, err := arena.lower()
	if err != nil {
		return err
	}
	return arrowed(C.ustore_docs_column_view_to_arrow(column.raw, C.ustore_size_t(column.rows), C.ustore_docs_field_type_t(fieldType), outputs, (*C.struct_ArrowSchema)(schema), (*C.struct_ArrowDeviceArray)(array)))
}
