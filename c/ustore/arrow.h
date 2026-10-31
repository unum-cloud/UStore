/**
 *  @file c/ustore/arrow.h
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The binary interface between Apache Arrow and the views every call takes and answers.
 *
 *  Converters read one Arrow column into a @c ustore_keys_view_t or a @c ustore_strings_view_t in place, and exporters
 *  hand the keys, tapes and bitmaps a call wrote into an arena out as Arrow columns that pin that arena. Loops over
 *  whole collections, record batches and schemas belong to their callers: a cursor and the exporters move a collection
 *  out, and the converters and the writes, or an ingest of @c ustore/db.h, move one in.
 *
 *  Keys are @c fixed_size_binary(16), tagged as the @c arrow.uuid extension or not, whose bytes are those of
 *  @c ustore_key_t, and byte strings are @c binary or @c large_binary, or @c utf8 and @c large_utf8, which share
 *  their layout. No column may hold a null, a dictionary or a view.
 *
 *  The structures of the Arrow C data, stream and device interfaces follow verbatim, under the guards Arrow defines, so
 *  a program including another copy compiles either.
 */

#ifndef UNUM_USTORE_ARROW_H
#define UNUM_USTORE_ARROW_H

#include "ustore/db.h"

#ifdef __cplusplus
extern "C" {
#endif

// clang-format off

#ifndef ARROW_C_DATA_INTERFACE
#  define ARROW_C_DATA_INTERFACE

#  define ARROW_FLAG_DICTIONARY_ORDERED 1
#  define ARROW_FLAG_NULLABLE 2
#  define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
  // Array type description
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct ArrowSchema** children;
  struct ArrowSchema* dictionary;

  // Release callback
  void (*release)(struct ArrowSchema*);
  // Opaque producer-specific data
  void* private_data;
};

struct ArrowArray {
  // Array data description
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct ArrowArray** children;
  struct ArrowArray* dictionary;

  // Release callback
  void (*release)(struct ArrowArray*);
  // Opaque producer-specific data
  void* private_data;
};

#endif  // ARROW_C_DATA_INTERFACE

#ifndef ARROW_C_DEVICE_DATA_INTERFACE
#  define ARROW_C_DEVICE_DATA_INTERFACE

// Spec and Documentation: https://arrow.apache.org/docs/format/CDeviceDataInterface.html

// DeviceType for the allocated memory
typedef int32_t ArrowDeviceType;

// CPU device, same as using ArrowArray directly
#  define ARROW_DEVICE_CPU 1
// CUDA GPU Device
#  define ARROW_DEVICE_CUDA 2
// Pinned CUDA CPU memory by cudaMallocHost
#  define ARROW_DEVICE_CUDA_HOST 3
// OpenCL Device
#  define ARROW_DEVICE_OPENCL 4
// Vulkan buffer for next-gen graphics
#  define ARROW_DEVICE_VULKAN 7
// Metal for Apple GPU
#  define ARROW_DEVICE_METAL 8
// Verilog simulator buffer
#  define ARROW_DEVICE_VPI 9
// ROCm GPUs for AMD GPUs
#  define ARROW_DEVICE_ROCM 10
// Pinned ROCm CPU memory allocated by hipMallocHost
#  define ARROW_DEVICE_ROCM_HOST 11
// Reserved for extension
#  define ARROW_DEVICE_EXT_DEV 12
// CUDA managed/unified memory allocated by cudaMallocManaged
#  define ARROW_DEVICE_CUDA_MANAGED 13
// unified shared memory allocated on a oneAPI non-partitioned device.
#  define ARROW_DEVICE_ONEAPI 14
// GPU support for next-gen WebGPU standard
#  define ARROW_DEVICE_WEBGPU 15
// Qualcomm Hexagon DSP
#  define ARROW_DEVICE_HEXAGON 16

struct ArrowDeviceArray {
  // the Allocated Array
  //
  // the buffers in the array (along with the buffers of any
  // children) are what is allocated on the device.
  struct ArrowArray array;
  // The device id to identify a specific device
  int64_t device_id;
  // The type of device which can access this memory.
  ArrowDeviceType device_type;
  // An event-like object to synchronize on if needed.
  void* sync_event;
  // Reserved bytes for future expansion.
  int64_t reserved[3];
};

#endif  // ARROW_C_DEVICE_DATA_INTERFACE

#ifndef ARROW_C_STREAM_INTERFACE
#  define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
  // Callback to get the stream type
  // (will be the same for all arrays in the stream).
  //
  // Return value: 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowSchema must be released independently from the stream.
  int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);

  // Callback to get the next array
  // (if no error and the array is released, the stream has ended)
  //
  // Return value: 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowArray must be released independently from the stream.
  int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);

  // Callback to get optional detailed error information.
  // This must only be called if the last stream operation failed
  // with a non-0 return code.
  //
  // Return value: pointer to a null-terminated character array describing
  // the last error, or NULL if no description is available.
  //
  // The returned pointer is only valid until the next operation on this stream
  // (including release).
  const char* (*get_last_error)(struct ArrowArrayStream*);

  // Release callback: release the stream's own resources.
  // Note that arrays returned by `get_next` must be individually released.
  void (*release)(struct ArrowArrayStream*);

  // Opaque producer-specific data
  void* private_data;
};

#endif  // ARROW_C_STREAM_INTERFACE

#ifndef ARROW_C_DEVICE_STREAM_INTERFACE
#  define ARROW_C_DEVICE_STREAM_INTERFACE

// Equivalent to ArrowArrayStream, but for ArrowDeviceArrays.
//
// This stream is intended to provide a stream of data on a single
// device, if a producer wants data to be produced on multiple devices
// then multiple streams should be provided. One per device.
struct ArrowDeviceArrayStream {
  // The device that this stream produces data on.
  ArrowDeviceType device_type;

  // Callback to get the stream schema
  // (will be the same for all arrays in the stream).
  //
  // Return value 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowSchema must be released independently from the stream.
  // The schema should be accessible via CPU memory.
  int (*get_schema)(struct ArrowDeviceArrayStream* self, struct ArrowSchema* out);

  // Callback to get the next array
  // (if no error and the array is released, the stream has ended)
  //
  // Return value: 0 if successful, an `errno`-compatible error code otherwise.
  //
  // If successful, the ArrowDeviceArray must be released independently from the stream.
  int (*get_next)(struct ArrowDeviceArrayStream* self, struct ArrowDeviceArray* out);

  // Callback to get optional detailed error information.
  // This must only be called if the last stream operation failed
  // with a non-0 return code.
  //
  // Return value: pointer to a null-terminated character array describing
  // the last error, or NULL if no description is available.
  //
  // The returned pointer is only valid until the next operation on this stream
  // (including release).
  const char* (*get_last_error)(struct ArrowDeviceArrayStream* self);

  // Release callback: release the stream's own resources.
  // Note that arrays returned by `get_next` must be individually released.
  void (*release)(struct ArrowDeviceArrayStream* self);

  // Opaque producer-specific data
  void* private_data;
};

#endif  // ARROW_C_DEVICE_STREAM_INTERFACE

// clang-format on

/**
 *  @brief Views the 16-byte keys of one Arrow @c fixed_size_binary(16) column in place, tagged @c arrow.uuid or not.
 *  @param[in] schema The column's schema.
 *  @param[in] array The column, whose @c offset is applied; only its lengths and pointers are read, never its buffers,
 *      so the @c array of an @c ArrowDeviceArray converts as well.
 *  @param[out] keys The keys, one every 16 bytes.
 *  @param[out] count The number of keys.
 *  @return @c ustore_status_invalid_argument_k for a missing or released input or one holding a null, which a
 *      @c null_count of -1 with a validity buffer may, @c ustore_status_unsupported_k for another format, a dictionary
 *      or a view, @c ustore_status_malformed_k for a negative length or offset or a missing buffer.
 */
USTORE_EXPORT ustore_status_t ustore_keys_view_from_arrow(struct ArrowSchema const* schema,
                                                          struct ArrowArray const* array, ustore_keys_view_t* keys,
                                                          ustore_size_t* count);

/**
 *  @brief Views the byte strings of one Arrow column in place: @c binary or @c utf8 as a tape of 32-bit offsets, and
 *      @c large_binary or @c large_utf8 as one of 64-bit offsets, starting at the column's @c offset.
 *  @return The statuses of @c ustore_keys_view_from_arrow.
 */
USTORE_EXPORT ustore_status_t ustore_strings_view_from_arrow(struct ArrowSchema const* schema,
                                                             struct ArrowArray const* array,
                                                             ustore_strings_view_t* strings, ustore_size_t* count);

/**
 *  @brief Exports @p count keys a call wrote into @p arena as one Arrow @c fixed_size_binary(16) column tagged as
 *      @c arrow.uuid, in place.
 *
 *  The array pins @p arena until its @c release: meanwhile a call naming the arena is refused with
 *  @c ustore_status_contended_k, and @c ustore_arena_free leaves the arena to the last release. Arenas on host pages
 *  export as @c ARROW_DEVICE_CPU. The schema's @c name is empty, for the caller to point at its own.
 *
 *  Keys of another stride, like the triples of edges, are copied dense into @p arena first, under the claim a call
 *  takes, so no other call or carving export runs on @p arena meanwhile.
 *
 *  @return @c ustore_status_invalid_argument_k for a missing output or dense keys outside @p arena,
 *      @c ustore_status_contended_k while a call fills @p arena or another export carves it,
 *      @c ustore_status_out_of_memory_k.
 */
USTORE_EXPORT ustore_status_t ustore_keys_view_to_arrow(ustore_keys_view_t keys, ustore_size_t count,
                                                        ustore_arena_t arena, struct ArrowSchema* schema,
                                                        struct ArrowDeviceArray* array);

/**
 *  @brief Exports a tape of @p count strings a call wrote into @p arena as one Arrow column of @p layout, in place
 *      when the tape already has it.
 *
 *  A @c ustore_strings_u32_tape_k layout exports as @c binary and a @c ustore_strings_u64_tape_k one as
 *  @c large_binary, pinning @p arena as @c ustore_keys_view_to_arrow does. A tape of the other width has its offsets
 *  converted into @p arena first, under the claim, narrowed ones counted from the tape's first byte.
 *  @return The statuses of @c ustore_keys_view_to_arrow, with an accessor or an unknown @p layout refused as an invalid
 *      argument, and @c ustore_status_capacity_exhausted_k for strings spanning more bytes than 32-bit offsets reach.
 */
USTORE_EXPORT ustore_status_t ustore_strings_view_to_arrow(ustore_strings_view_t strings, ustore_size_t count,
                                                           ustore_strings_layout_t layout, ustore_arena_t arena,
                                                           struct ArrowSchema* schema, struct ArrowDeviceArray* array);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_ARROW_H
