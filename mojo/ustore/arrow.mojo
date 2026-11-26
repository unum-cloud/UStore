"""Apache Arrow through its C data interface: the keys, strings and gathered columns a call wrote into
an arena exported in place, and Arrow columns read in place as the inputs of a call.

Until an export is released, the arena's next call raises `CONTENDED` and its free frees nothing,
and its release frees only the array's own nodes, which an `ArrowExport` runs when it goes out of
scope. The schema
and array addresses an export answers are `struct ArrowSchema*` and `struct ArrowDeviceArray*`,
for any consumer of the C data interface to import.

File: mojo/ustore/arrow.mojo
Author: Ash Vardanian
Date: November 16, 2026
"""

from std.ffi import c_int, c_size_t
from std.memory import stack_allocation
from std.sys.info import CompilationTarget

from ustore.db import Key, KeysView, StridedArguments
from ustore.docs import Column, ColumnViewArguments, FieldType
from ustore.library import Library
from ustore.types import (
    Address,
    Arena,
    Error,
    NULL_ADDRESS,
    Status,
    StringsView,
    StringsViewArguments,
    Tape,
    _viewed,
)

# region Layouts


@fieldwise_init
struct ArrowSchema(Defaultable, ImplicitlyCopyable):
    """`struct ArrowSchema`, laid out as the Arrow C data interface declares it."""

    var format: Address
    var name: Address
    var metadata: Address
    var flags: Int64
    var n_children: Int64
    var children: Address
    var dictionary: Address
    var release: Address
    var private_data: Address

    def __init__(out self):
        """A released schema, which is what a failed export leaves."""
        self = Self(
            NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, 0, 0, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS
        )


@fieldwise_init
struct ArrowArray(Defaultable, ImplicitlyCopyable):
    """`struct ArrowArray`, laid out as the Arrow C data interface declares it."""

    var length: Int64
    var null_count: Int64
    var offset: Int64
    var n_buffers: Int64
    var n_children: Int64
    var buffers: Address
    var children: Address
    var dictionary: Address
    var release: Address
    var private_data: Address

    def __init__(out self):
        """A released array, which is what a failed export leaves."""
        self = Self(0, 0, 0, 0, 0, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS, NULL_ADDRESS)


@fieldwise_init
struct ArrowDeviceArray(Defaultable, ImplicitlyCopyable):
    """`struct ArrowDeviceArray`, laid out as the Arrow C device interface declares it."""

    var array: ArrowArray
    var device_id: Int64
    var device_type: Int32
    var _padding: Int32
    """The four bytes C aligns `sync_event` past."""
    var sync_event: Address
    var reserved0: Int64
    var reserved1: Int64
    var reserved2: Int64

    def __init__(out self):
        """A released array on no device, which is what a failed export leaves."""
        self = Self(ArrowArray(), -1, 0, 0, NULL_ADDRESS, 0, 0, 0)


# endregion Layouts

# region Signatures

comptime _Schema = Pointer[ArrowSchema, MutAnyOrigin]
comptime _DeviceArray = Pointer[ArrowDeviceArray, MutAnyOrigin]
comptime _Array = Pointer[ArrowArray, MutAnyOrigin]
comptime _Count = Pointer[c_size_t, MutAnyOrigin]
comptime _SchemaRelease = def(_Schema) thin abi("C") -> None
comptime _ArrayRelease = def(_Array) thin abi("C") -> None

comptime KeysFromArrow = def(_Schema, _Array, Pointer[StridedArguments, MutAnyOrigin], _Count) thin abi("C") -> c_int
"""`ustore_keys_view_from_arrow`: a schema, an array, then the keys view and the count it writes.

Typed pointers rather than `Address` for every output, as `VersionOf` records."""

comptime StringsFromArrow = def(_Schema, _Array, Pointer[StringsViewArguments, MutAnyOrigin], _Count) thin abi(
    "C"
) -> c_int
"""`ustore_strings_view_from_arrow`: a schema, an array, then the strings view and the count it writes."""

comptime KeysToArrow = def(StridedArguments, c_size_t, Address, _Schema, _DeviceArray) thin abi("C") -> c_int
"""`ustore_keys_view_to_arrow`: the keys, their count and arena, then the schema and array it fills."""

comptime StringsToArrow = def(
    Pointer[StringsViewArguments, MutAnyOrigin], c_size_t, c_int, Address, _Schema, _DeviceArray
) thin abi("C") -> c_int
"""`ustore_strings_view_to_arrow`: the tape, its count, the layout, the arena, then the schema and array.

The 24-byte `ustore_strings_view_t` crosses as the address of a copy, which is how AAPCS64 passes a
composite past 16 bytes by value."""

comptime ColumnToArrow = def(
    Pointer[ColumnViewArguments, MutAnyOrigin], c_size_t, c_int, Address, _Schema, _DeviceArray
) thin abi("C") -> c_int
"""`ustore_docs_column_view_to_arrow`: the column, its rows, its type, the arena, then the schema and array."""

# endregion Signatures

# region Exports


struct ArrowExport(Movable, Sized):
    """One Arrow column a call's arena holds, exported in place, released when it goes out of scope.

    The arena must outlive it: until it is released, the arena's next call raises `CONTENDED`, and
    an arena freed earlier frees nothing, staying allocated until the process ends.
    """

    var schema: ArrowSchema
    """The column's type, its name left empty for the caller to point at its own."""
    var array: ArrowDeviceArray
    """The column's buffers and the device they live on."""
    var _library: Library
    """The loaded engine, held so the releases, which live in it, still run."""

    def __init__(out self, library: Library, schema: ArrowSchema, array: ArrowDeviceArray):
        """Takes over the nodes an export call filled, whose releases this export runs."""
        self.schema = schema
        self.array = array
        self._library = library

    def __deinit__(deinit self):
        """Releases the array's and the schema's own nodes through their release callbacks, if any."""
        if self.array.array.release != NULL_ADDRESS:
            var release = Pointer(to=self.array.array.release).unsafe_bitcast[_ArrayRelease]()[]
            release(_Array(to=self.array.array))
        if self.schema.release != NULL_ADDRESS:
            var release = Pointer(to=self.schema.release).unsafe_bitcast[_SchemaRelease]()[]
            release(_Schema(to=self.schema))
        _ = self._library^

    def __len__(self) -> Int:
        """The rows the column holds."""
        return Int(self.array.array.length)

    def device_type(self) -> Int:
        """The `ARROW_DEVICE_*` type of the device holding the buffers, 1 for the host."""
        return Int(self.array.device_type)

    def schema_address(ref self) -> Address:
        """The `struct ArrowSchema*` a consumer imports, valid while this export lives, so a caller keeps
        the export alive past its last use of the address, as Mojo frees a value at its last use."""
        return Int(Pointer(to=self.schema))

    def array_address(ref self) -> Address:
        """The `struct ArrowDeviceArray*` a consumer imports, valid while this export lives."""
        return Int(Pointer(to=self.array))


def _exported(status: c_int, detail: StaticString) raises Error:
    """Raises the status an export or a view answered, its message being the call's name."""
    if Status(Int32(status)) != Status.SUCCESS:
        raise Error(Status(Int32(status)), detail)


def keys_to_arrow(keys: KeysView, arena: Arena) raises Error -> ArrowExport:
    """Exports the keys a call wrote into `arena` as one `fixed_size_binary(16)` column tagged `arrow.uuid`.
    Calls `ustore_keys_view_to_arrow`."""
    var schema = stack_allocation[1, ArrowSchema]().unsafe_origin_cast[MutAnyOrigin]()
    schema[] = ArrowSchema()
    var array = stack_allocation[1, ArrowDeviceArray]().unsafe_origin_cast[MutAnyOrigin]()
    array[] = ArrowDeviceArray()
    var status = arena._library.symbols().keys_view_to_arrow(
        keys.arguments(), c_size_t(len(keys)), arena._address, schema, array
    )
    _exported(status, "ustore_keys_view_to_arrow")
    return ArrowExport(arena._library, schema[], array[])


def tape_to_arrow(tape: Tape) raises Error -> ArrowExport:
    """Exports a tape a call wrote into its arena in place, as `binary` for 32-bit offsets or `large_binary`
    for 64-bit ones. Calls `ustore_strings_view_to_arrow`.

    Raises:
        `Error` with `UNSUPPORTED` on x86, whose ABI passes the view by value on the stack, which no typed
            pointer spells.
    """
    if CompilationTarget.is_x86():
        raise Error(Status.UNSUPPORTED, "ustore_strings_view_to_arrow")
    var copied = stack_allocation[1, StringsViewArguments]().unsafe_origin_cast[MutAnyOrigin]()
    copied[] = tape.view
    var schema = stack_allocation[1, ArrowSchema]().unsafe_origin_cast[MutAnyOrigin]()
    schema[] = ArrowSchema()
    var array = stack_allocation[1, ArrowDeviceArray]().unsafe_origin_cast[MutAnyOrigin]()
    array[] = ArrowDeviceArray()
    var status = (
        tape.arena[]
        ._library.symbols()
        .strings_view_to_arrow(copied, c_size_t(len(tape)), tape.view.layout, tape.arena[]._address, schema, array)
    )
    _exported(status, "ustore_strings_view_to_arrow")
    return ArrowExport(tape.arena[]._library, schema[], array[])


def column_to_arrow(column: Column, type: FieldType) raises Error -> ArrowExport:
    """Exports one gathered column of `type`, the type the gather asked for it, with its validities.
    Calls `ustore_docs_column_view_to_arrow`."""
    var view = stack_allocation[1, ColumnViewArguments]().unsafe_origin_cast[MutAnyOrigin]()
    view[] = column.view
    var schema = stack_allocation[1, ArrowSchema]().unsafe_origin_cast[MutAnyOrigin]()
    schema[] = ArrowSchema()
    var array = stack_allocation[1, ArrowDeviceArray]().unsafe_origin_cast[MutAnyOrigin]()
    array[] = ArrowDeviceArray()
    var status = (
        column.arena[]
        ._library.symbols()
        .docs_column_view_to_arrow(view, c_size_t(len(column)), type.code, column.arena[]._address, schema, array)
    )
    _exported(status, "ustore_docs_column_view_to_arrow")
    return ArrowExport(column.arena[]._library, schema[], array[])


# endregion Exports

# region Imports


def keys_from_arrow(arena: Arena, schema: Address, array: Address) raises Error -> KeysView[ImmUntrackedOrigin]:
    """Views the keys of one Arrow `fixed_size_binary(16)` column in place, as a call's input, which the
    column must outlive. `arena` names the loaded library. Calls `ustore_keys_view_from_arrow`."""
    var keys = stack_allocation[1, StridedArguments]().unsafe_origin_cast[MutAnyOrigin]()
    keys[] = StridedArguments()
    var count = stack_allocation[1, c_size_t]().unsafe_origin_cast[MutAnyOrigin]()
    count[] = 0
    _exported(
        arena._library.symbols().keys_view_from_arrow(
            _Schema(unsafe_from_address=schema), _Array(unsafe_from_address=array), keys, count
        ),
        "ustore_keys_view_from_arrow",
    )
    return KeysView(_viewed[Key, ImmUntrackedOrigin](keys[].data), Int(keys[].stride), Int(count[]))


def strings_from_arrow(arena: Arena, schema: Address, array: Address) raises Error -> StringsView[ImmUntrackedOrigin]:
    """Views the strings of one Arrow `binary` or `utf8` column, or their large kinds, in place, as a
    call's input, which the column must outlive. `arena` names the loaded library. Calls
    `ustore_strings_view_from_arrow`."""
    var strings = stack_allocation[1, StringsViewArguments]().unsafe_origin_cast[MutAnyOrigin]()
    strings[] = StringsViewArguments()
    var count = stack_allocation[1, c_size_t]().unsafe_origin_cast[MutAnyOrigin]()
    count[] = 0
    var status = arena._library.symbols().strings_view_from_arrow(
        _Schema(unsafe_from_address=schema), _Array(unsafe_from_address=array), strings, count
    )
    _exported(status, "ustore_strings_view_from_arrow")
    var view = strings[]
    return StringsView(_viewed[UInt8, ImmUntrackedOrigin](view.data), view.offsets, view.layout, Int(count[]))


# endregion Imports
