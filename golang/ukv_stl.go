package ukv_stl

/*
#cgo CFLAGS: -g -Wall -I${SRCDIR}/../include
#cgo LDFLAGS: -L${SRCDIR}/../lib -lukv_stl -lstdc++

#include "ukv.h"
#include <stdlib.h>

ukv_val_len_t tape_get_length(ukv_tape_ptr_t tape, ukv_size_t key_idx) {
	return ((ukv_val_len_t *)tape)[key_idx]; }
ukv_tape_ptr_t* tape_get_value(ukv_tape_ptr_t tape, ukv_size_t count_keys, ukv_size_t bytes_to_skip) {
	return (ukv_tape_ptr_t *)(tape + sizeof(ukv_val_len_t) * count_keys) + bytes_to_skip; }
*/
import "C"
import (
	"errors"
	"unsafe"
)

/**
 * This class is modeled after Redis client and other ORMs:
 * https://github.com/go-redis/redis
 * https://bun.uptrace.dev
 *
 * Further Reading:
 * https://blog.marlin.org/cgo-referencing-c-library-in-go
 */
type DataBase struct {
	raw C.ukv_t
}

func forwardError(error_c C.ukv_error_t) error {
	if error_c != nil {
		error_go := C.GoString(error_c)
		C.ukv_error_free(error_c)
		return errors.New(error_go)
	}
	return nil
}

func cleanTape(db *DataBase, tape_c C.ukv_tape_ptr_t, tape_length_c C.ukv_size_t) {
	C.ukv_tape_free(db.raw, tape_c, tape_length_c)
}

func (db *DataBase) ReConnect(config string) error {

	error_c := C.ukv_error_t(nil)
	config_c := C.CString(config)
	defer C.free(unsafe.Pointer(config_c))

	C.ukv_open(config_c, &db.raw, &error_c)
	return forwardError(error_c)
}

func (db *DataBase) Close() {
	if db.raw != nil {
		C.ukv_free(db.raw)
		db.raw = nil
	}
}

func (db *DataBase) Set(key uint64, value *[]byte) error {

	// Passing values without copies seems essentially impossible
	// and causes: "cgo argument has Go pointer to Go pointer"
	// when we try to take pass pointer to to first object in `[]byte`.
	// https://stackoverflow.com/a/64867672
	error_c := C.ukv_error_t(nil)
	key_c := C.ukv_key_t(key)
	collection_c := (*C.ukv_collection_t)(nil)
	options_c := C.ukv_options_t(C.ukv_options_default_k)
	value_go := C.CBytes(*value)
	value_ptr_c := C.ukv_tape_ptr_t(value_go)
	value_length_c := C.ukv_val_len_t(len(*value))
	defer C.free(value_go)

	C.ukv_write(db.raw, nil, collection_c, 0, &key_c, 1, 0, &value_ptr_c, 0, &value_length_c, 0, options_c, &error_c)
	return forwardError(error_c)
}

func (db *DataBase) Delete(key uint64) error {

	// Passing values without copies seems essentially impossible
	// and causes: "cgo argument has Go pointer to Go pointer"
	// when we try to take pass pointer to to first object in `[]byte`.
	// https://stackoverflow.com/a/64867672
	error_c := C.ukv_error_t(nil)
	key_c := C.ukv_key_t(key)
	collection_c := (*C.ukv_collection_t)(nil)
	options_c := C.ukv_options_t(C.ukv_options_default_k)
	value_ptr_c := C.ukv_tape_ptr_t(nil)
	value_length_c := C.ukv_val_len_t(0)

	C.ukv_write(db.raw, nil, collection_c, 0, &key_c, 1, 0, &value_ptr_c, 0, &value_length_c, 0, options_c, &error_c)
	return forwardError(error_c)
}

func (db *DataBase) Get(key uint64) ([]byte, error) {

	// Even though we can't properly write without a single copy
	// from Go layer, but we can read entries from C-allocated buffers.
	error_c := C.ukv_error_t(nil)
	key_c := C.ukv_key_t(key)
	collection_c := (*C.ukv_collection_t)(nil)
	options_c := C.ukv_options_t(C.ukv_options_default_k)
	tape_c := (C.ukv_tape_ptr_t)(nil)
	tape_length_c := (C.ukv_size_t)(0)

	C.ukv_read(db.raw, nil,
		collection_c, 0,
		&key_c, 1, 0,
		options_c,
		&tape_c, &tape_length_c,
		&error_c)

	error_go := forwardError(error_c)
	if error_go != nil {
		return nil, error_go
	}

	pulled_value_length_c := C.tape_get_length(tape_c, 0)
	pulled_value_c := C.tape_get_value(tape_c, 1, 0)

	// We can also jsut cast it to slice without copies
	// pulled_value_go := (*[1 << 30]byte)(unsafe.Pointer(pulled_value_c))[:tape_length_c:tape_length_c]
	pulled_value_go := C.GoBytes(unsafe.Pointer(pulled_value_c), C.int(pulled_value_length_c))
	return pulled_value_go, nil

}

func (db *DataBase) Contains(key uint64) (bool, error) {

	// Even though we can't properly write without a single copy
	// from Go layer, but we can read entries from C-allocated buffers.
	error_c := C.ukv_error_t(nil)
	key_c := C.ukv_key_t(key)
	collection_c := (*C.ukv_collection_t)(nil)
	options_c := C.ukv_options_t(C.ukv_option_read_lengths_k)
	tape_c := (C.ukv_tape_ptr_t)(nil)
	tape_length_c := (C.ukv_size_t)(0)
	defer cleanTape(db, tape_c, tape_length_c)

	C.ukv_read(db.raw, nil,
		collection_c, 0,
		&key_c, 1, 0,
		options_c,
		&tape_c, &tape_length_c,
		&error_c)

	error_go := forwardError(error_c)
	if error_go != nil {
		return false, error_go
	}

	pulled_value_length_c := C.tape_get_length(tape_c, 0)
	return pulled_value_length_c != 0, nil
}
