"""The vocabulary every verb shares: statuses, views, executors, arenas, callbacks, and the one error type.

`Error` is the only error type the binding raises: Mojo allows at most one per function, so a second
would force every caller raising `Error` to catch and convert. A refusal is a failure, not an
absence, so a call the engine declines raises.

`MutAnyOrigin` rather than `MutUntrackedOrigin` is load-bearing on anything C writes through: with
a narrower origin the compiler folds a read of the slot back to whatever the caller last stored
there, and every out-parameter reads back as zero.

A struct whose address crosses to C is materialized into a `stack_allocation` slot rather than held
in a local whose address is taken. An `alloca` is not a value Mojo owns, so nothing destroys it and
nothing folds its store away, where a local's last tracked use is the address-taking itself. A
deferred call's slots outlive the verb that submitted them, so they live on the heap instead.

File: mojo/ustore/types.mojo
Author: Ash Vardanian
Date: November 8, 2026
"""

from std.ffi import c_char, c_int, c_size_t
from std.memory import ArcPointer, OwnedPointer, stack_allocation
from std.utils import StaticTuple

from ustore.db import Scope
from ustore.library import Library

# region Pointers

comptime Address = Int
"""An opaque C handle or untyped pointer crossing as its numeric value, which is how a null one is
expressed. The integer is pointer-width and shares the pointer's argument class on both x86-64 and
AArch64, so it lands in the same register a pointer would."""

comptime NULL_ADDRESS = 0
"""The null pointer, as `Address` spells it."""

comptime CString = Pointer[c_char, ImmUntrackedOrigin]
"""A static null-terminated C string the engine owns, as the name tables and messages hand them out."""


def _anchor[origin: Origin]() -> ref[origin] NoneType:
    """A reference of the given origin, read only inside `origin_of` to union parameter origins."""
    return Pointer[NoneType, origin](unsafe_from_address=NULL_ADDRESS)[]


def _borrowed[T: AnyType, origin: Origin](address: Address) -> Optional[Pointer[T, origin]]:
    """A borrowed pointer of the given origin, absent where the address is null, as an empty run's may be."""
    if address == NULL_ADDRESS:
        return None
    return Pointer[T, origin](unsafe_from_address=address)


def _address_of[T: AnyType, origin: Origin, //](pointer: Optional[Pointer[T, origin]]) -> Address:
    """The numeric value of a pointer that may be absent, as the C layout spells it."""
    if case .Some(present) = pointer:
        return Int(present)
    return NULL_ADDRESS


# endregion Pointers

# region Errors


@fieldwise_init
struct Status(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Why a call into the engine failed, the `ustore_status_t` vocabulary verbatim.

    An `Error` never carries `SUCCESS` or `DEFERRED`: only a `Deferred` reports the latter, and only
    until its call is final. `Library.status_name` spells each one as the engine does.
    """

    var code: Int32
    """The raw `ustore_status_t` value, kept even when this build does not name it."""
    comptime SUCCESS = Self(0)
    """The call succeeded."""
    comptime DEFERRED = Self(1)
    """The call was submitted deferred and is not final yet; the one non-final status."""
    comptime OUT_OF_MEMORY = Self(2)
    """An allocation could not be satisfied."""
    comptime READ_ONLY = Self(6)
    """The call would change a database opened read-only."""
    comptime IO = Self(7)
    """A file or directory could not be opened, read, written, or closed."""
    comptime DEVICE = Self(8)
    """A device could not be used, or the executor, database and arena live in different places."""
    comptime MALFORMED = Self(9)
    """Stored or supplied data could not be decoded."""
    comptime INTERNAL = Self(10)
    """The engine failed in a way no other status names, which points at a defect."""
    comptime UNINITIALIZED = Self(12)
    """The call needs a database or transaction that was never opened."""
    comptime CONTENDED = Self(14)
    """The call was refused rather than waited for, like a submission to a full deferred table."""
    comptime CANCELLED = Self(15)
    """A progress handler stopped the call, which unwound and published nothing."""
    comptime UNSUPPORTED = Self(16)
    """The engine or build does not implement what the call asks for."""
    comptime INVALID_ARGUMENT = Self(17)
    """An argument is null, contradicts another one, or carries a choice the call does not accept."""
    comptime NOT_FOUND = Self(18)
    """A collection, a directory, or a document a modification needs does not exist."""
    comptime ALREADY_EXISTS = Self(19)
    """A collection name or a document the call creates is already taken."""
    comptime WRITE_CONFLICT = Self(20)
    """A concurrent writer changed a key this transaction wrote; retry the transaction."""
    comptime READ_CONFLICT = Self(21)
    """A concurrent writer changed a key this transaction read; retry the transaction."""
    comptime PHANTOM_CONFLICT = Self(22)
    """A concurrent writer inserted into a range this transaction scanned; retry the transaction."""
    comptime CAPACITY_EXHAUSTED = Self(23)
    """A hard design ceiling was reached."""
    comptime LIBRARY_MISSING = Self(-1)
    """Nothing to `dlopen`, the binding's own."""
    comptime SYMBOL_MISSING = Self(-2)
    """The library loaded but does not export a symbol the binding needs, the binding's own."""
    comptime VERSION_MISMATCH = Self(-3)
    """The library loaded but reports a different major version than the binding's, the binding's own."""

    @staticmethod
    def of(code: c_int) -> Self:
        """Lifts a raw `ustore_status_t`, keeping an unnamed one rather than guessing."""
        return Self(Int32(code))


@fieldwise_init
struct Outcome(ImplicitlyCopyable):
    """The `ustore_outcome_t` every verb reports through, laid out as the ABI declares it."""

    var status: c_int
    """The `ustore_status_t` the call reported."""
    var message: Optional[CString]
    """The engine's own static message, absent when the call succeeded."""

    def __init__(out self):
        """Zeroes the slot, so a call that writes nothing still reads as a success."""
        self.status = c_int(0)
        self.message = None


@fieldwise_init
struct Error(Copyable, ImplicitlyCopyable, Writable):
    """What went wrong inside the engine, and which entry point it was about."""

    var status: Status
    """Which class of failure the engine reported."""
    var detail: StaticString
    """The entry point it was about, for a reader rather than for a branch."""
    var message: Optional[CString]
    """The engine's own static message, which outlives the call and needs no freeing."""
    var name: Optional[CString]
    """The engine's spelling of `status`, absent where no engine named it."""
    var library: Optional[Library]
    """The engine `message` and `name` live in, kept mapped for as long as the error is; Mojo would
    otherwise unload it at the last handle's last use, while the error is still being read."""

    def __init__(out self, status: Status, detail: StaticString):
        """A failure the binding caught before any engine could name it.

        Args:
            status: Which class of failure it is.
            detail: The entry point or binding type it was about.
        """
        self.status = status
        self.detail = detail
        self.message = None
        self.name = None
        self.library = None

    def write_to(self, mut writer: Some[Writer]):
        """Writes the status name, the entry point, and the engine's own message together.

        Args:
            writer: The sink the message is appended to.
        """
        writer.write("UStore: ")
        if case .Some(name) = self.name:
            writer.write(text_at(name))
        else:
            match self.status:
                case == Status.LIBRARY_MISSING:
                    writer.write("library-missing")
                case == Status.SYMBOL_MISSING:
                    writer.write("symbol-missing")
                case == Status.VERSION_MISMATCH:
                    writer.write("version-mismatch")
                case _:
                    writer.write("status ", self.status.code)
        writer.write(" [", self.detail, "]")
        if case .Some(message) = self.message:
            writer.write(": ", text_at(message))


def text_at(text: CString) -> String:
    """Copies a null-terminated C string the engine owns, stopping at its terminator."""
    var bytes = text.unsafe_bitcast[UInt8]()
    var length = 0
    while bytes[unsafe_offset=length] != 0:
        length += 1
    return String(StringSlice(unsafe_from_utf8=Span[UInt8, ImmUntrackedOrigin](unsafe_ptr=bytes, length=length)))


# endregion Errors

# region Execution


@fieldwise_init
struct Completion(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Whether a verb returns with its final status or once submitted, `ustore_completion_t`.

    The receiver picks it: a handle and `handle.on(executor)` pass `BLOCKING`, and
    `handle.deferred(executor)` passes `DEFERRED`, so no verb takes it as an argument.
    """

    var code: c_int
    """The raw `ustore_completion_t` value the ABI exchanges."""
    comptime BLOCKING = Self(0)
    """Return with the final status stored and every output readable."""
    comptime DEFERRED = Self(1)
    """Return once submitted; the status reads `DEFERRED` until final."""


@fieldwise_init
struct Mood(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """What an executor's threads or rings do once it goes idle, `ustore_mood_t`."""

    var code: c_int
    """The raw `ustore_mood_t` value the ABI exchanges."""
    comptime PARKED = Self(0)
    """Parked, so the next call pays a wake-up."""
    comptime SPINNING = Self(1)
    """Spinning, trading power for the next call's latency."""


@fieldwise_init
struct CPUClass(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The CPU generation a database or an executor confines itself to, `ustore_cpu_class_t`."""

    var code: c_int
    """The raw `ustore_cpu_class_t` value the ABI exchanges."""
    comptime UNKNOWN = Self(0)
    """Whatever the running CPU offers."""
    comptime SERIAL = Self(1)
    """The portable floor every CPU runs."""
    comptime INTEL_HASWELL = Self(2)
    """Intel and AMD x86 cores from Haswell on."""
    comptime INTEL_SIERRA = Self(3)
    """Intel x86 efficiency cores from Sierra Forest on."""
    comptime INTEL_SKYLAKE = Self(4)
    """Intel x86 server cores from Skylake on."""
    comptime INTEL_ICELAKE = Self(5)
    """Intel x86 server cores from Ice Lake on."""
    comptime AWS_G2 = Self(6)
    """Arm cores from AWS Graviton 2 on."""
    comptime APPLE_M2 = Self(7)
    """Arm cores from Apple M2 on."""
    comptime AWS_G5 = Self(8)
    """Arm server cores from AWS Graviton 5 on."""
    comptime ALIBABA_C930 = Self(9)
    """RISC-V cores with RVV, Zawrs and Zacas, from Alibaba's C930 on."""
    comptime MONOVERSIONED = Self(10)
    """The one class of a monoversioned build."""


@fieldwise_init
struct Transport(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """How the NVMe engine moves bytes between its files and memory, `ustore_transport_t`."""

    var code: c_int
    """The raw `ustore_transport_t` value the ABI exchanges."""
    comptime AUTOMATIC = Self(0)
    """Linux io_uring where the kernel offers it, Linux AIO where it offers only that, and blocking calls elsewhere."""
    comptime BLOCKING = Self(1)
    """Positioned reads and writes that block the polling thread."""
    comptime LINUX_URING = Self(2)
    """Linux io_uring on unbuffered files."""
    comptime LINUX_AIO = Self(3)
    """Linux AIO on unbuffered files."""
    comptime LINUX_SPDK = Self(4)
    """SPDK's user-space driver on NVMe devices the process owns."""


@fieldwise_init
struct Gds(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """How a VRAM executor moves bytes between the drives and its device, `ustore_gds_t`."""

    var code: c_int
    """The raw `ustore_gds_t` value the ABI exchanges."""
    comptime AUTOMATIC = Self(0)
    """Direct transfers where the driver is loaded, and the compatibility mode elsewhere."""
    comptime DIRECT = Self(1)
    """Direct transfers, refused with `DEVICE` where the driver is absent."""
    comptime COMPATIBILITY = Self(2)
    """The compatibility mode, which bounces through host pages it manages."""


@fieldwise_init
struct GPUVendor(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The GPU vendors one binary drives at once, `ustore_gpu_vendor_t`."""

    var code: c_int
    """The raw `ustore_gpu_vendor_t` value the ABI exchanges."""
    comptime UNKNOWN = Self(0)
    """No vendor."""
    comptime NVIDIA = Self(1)
    """NVIDIA, through the CUDA driver."""
    comptime AMD = Self(2)
    """AMD, through the HIP runtime."""


@fieldwise_init
struct GPUClass(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """The GPU generations UStore tells apart, `ustore_gpu_class_t`."""

    var code: c_int
    """The raw `ustore_gpu_class_t` value the ABI exchanges."""
    comptime UNKNOWN = Self(0)
    """No class."""
    comptime NVIDIA_AMPERE = Self(1)
    """Ampere-class NVIDIA and every NVIDIA GPU without a class of its own."""
    comptime NVIDIA_ADA = Self(2)
    """Ada-class NVIDIA."""
    comptime NVIDIA_HOPPER = Self(3)
    """Hopper-class NVIDIA."""
    comptime NVIDIA_BLACKWELL = Self(4)
    """Blackwell-class NVIDIA datacenter parts."""
    comptime NVIDIA_BLACKWELLRTX = Self(5)
    """Consumer Blackwell NVIDIA."""
    comptime AMD_CDNA3 = Self(6)
    """CDNA3 AMD, gfx942."""
    comptime AMD_CDNA4 = Self(7)
    """CDNA4 AMD, gfx950."""
    comptime AMD_CDNA5 = Self(8)
    """CDNA5 AMD, gfx1250."""


struct GPUDevice(Copyable, Defaultable, Movable):
    """One GPU as UStore numbers it, `ustore_gpu_device_t`: its position in `Library.gpu_devices` is the
    ordinal a VRAM executor names it by."""

    var vendor: GPUVendor
    """The vendor whose runtime drives the device."""
    var vendor_ordinal: UInt32
    """The device's ordinal in its vendor's runtime."""
    var gpu_class: GPUClass
    """The class of the device."""
    var compute_units: UInt32
    """Streaming multiprocessors or compute units."""
    var memory_bytes: UInt64
    """Device memory in bytes."""
    var _name: StaticTuple[UInt8, 64]
    """The vendor's own name for the device, NUL-terminated and truncated to fit."""

    def __init__(out self):
        """An empty entry for the listing to fill."""
        self.vendor = GPUVendor.UNKNOWN
        self.vendor_ordinal = 0
        self.gpu_class = GPUClass.UNKNOWN
        self.compute_units = 0
        self.memory_bytes = 0
        self._name = StaticTuple[UInt8, 64](fill=0)

    def name(self) -> String:
        """The vendor's own name for the device."""
        return text_at(CString(unsafe_from_address=Int(Pointer(to=self._name))))


@fieldwise_init
struct Continuation(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Whether the call a progress handler is watching carries on or unwinds, `ustore_continuation_t`."""

    var code: c_int
    """The raw `ustore_continuation_t` value the ABI exchanges."""
    comptime CONTINUE = Self(0)
    """Carry on to the next unit of work."""
    comptime CANCEL = Self(1)
    """Stop, publish nothing, and finish the call with `CANCELLED`."""


def _progress[
    T: AnyType, handler: def(Int, Int, Pointer[T, MutAnyOrigin]) thin -> Continuation
](finished: c_size_t, total: c_size_t, state: Pointer[T, MutAnyOrigin]) abi("C") -> c_int:
    """The C callback of one handler, instantiated per handler since a capturing closure cannot cross."""
    return handler(Int(finished), Int(total), state).code


@fieldwise_init
struct Progress(Defaultable, ImplicitlyCopyable):
    """How a long call reports its advance: the `progress` and `progress_state` pair of every verb that
    takes one. The handler never runs concurrently with itself, nor after its call is final."""

    var callback: Address
    """The handler's C entry point; `NULL_ADDRESS` skips every report."""
    var state: Address
    """Forwarded into every report; it must outlive the call."""

    def __init__(out self):
        """No handler: the call reports nothing."""
        self = Self(NULL_ADDRESS, NULL_ADDRESS)

    @staticmethod
    def of[
        T: AnyType, origin: MutOrigin, //, handler: def(Int, Int, Pointer[T, MutAnyOrigin]) thin -> Continuation
    ](state: Pointer[T, origin]) -> Self:
        """Reports to `handler` with `state`, which returns `Continuation.CANCEL` to stop the call.

        Parameters:
            T: What `state` points at, inferred from it.
            origin: The state's origin, inferred with it.
            handler: Called with the units finished, the units in total, and `state`.

        Args:
            state: Forwarded into every report; it must outlive the call.
        """
        var callback = _progress[T, handler]
        return Self(Pointer(to=callback).unsafe_bitcast[Int]()[], Int(state))


# endregion Execution

# region Signatures

comptime Verb[Arguments: AnyType] = def(Pointer[Arguments, MutAnyOrigin]) thin abi("C") -> None
"""Every struct verb: one argument structure named after it, its outputs written through its pointers."""

comptime Release = def(Address) thin abi("C") -> None
"""The positional `_free` family; `NULL` is safe."""

comptime OutcomeStatus = def(Pointer[Outcome, MutAnyOrigin]) thin abi("C") -> c_int
"""`ustore_outcome_wait` and `ustore_outcome_status`: an outcome's status, the first once it is final."""

comptime NameOf = def(c_int) thin abi("C") -> CString
"""The `_name` family: an enumerator's static spelling."""

comptime FromName = def(BytesViewArguments) thin abi("C") -> c_int
"""The `_from_name` family: reads a spelling back into its enumerator."""

comptime ViewOf = def(Address) thin abi("C") -> Address
"""`ustore_snapshot_view`: the view a handle names, allocating nothing."""

comptime WatchedViewOf = def(Address, c_int) thin abi("C") -> Address
"""`ustore_transaction_view`: the view a transaction names, recording its keys or not."""

comptime GPUDevices = Verb[GPUDevicesArguments]
"""`ustore_gpu_devices`."""
comptime ExecutorForRam = Verb[ExecutorForRamArguments]
"""`ustore_executor_for_ram`."""
comptime ExecutorOnPool = Verb[ExecutorOnPoolArguments]
"""`ustore_executor_on_pool`."""
comptime ExecutorForNvme = Verb[ExecutorForNvmeArguments]
"""`ustore_executor_for_nvme`."""
comptime ExecutorForVram = Verb[ExecutorForVramArguments]
"""`ustore_executor_for_vram`."""
comptime ExecutorDrain = Verb[ExecutorDrainArguments]
"""`ustore_executor_drain`."""
comptime ArenaInit = Verb[ArenaInitArguments]
"""`ustore_arena_init`."""
comptime ArenaDescribe = Verb[ArenaDescribeArguments]
"""`ustore_arena_describe`."""


def _cross[Arguments: ImplicitlyCopyable & Deinitable, //](arguments: Arguments, verb: Verb[Arguments]):
    """Materializes one argument structure in a stack slot and hands its address to `verb`."""
    var slot = stack_allocation[1, Arguments]()
    slot[unsafe_offset=0] = arguments
    verb(slot.unsafe_origin_cast[MutAnyOrigin]())


def _lent[T: AnyType, origin: MutOrigin, //](slot: Optional[Pointer[T, origin]]) -> Optional[Pointer[T, MutUntrackedOrigin]]:
    """A lent output slot with its borrow erased, as the C layout names it."""
    if case .Some(pointer) = slot:
        return pointer.unsafe_origin_cast[MutUntrackedOrigin]()
    return None


def _slot[T: AnyType](wanted: Bool, slot: Pointer[T, MutUntrackedOrigin]) -> Optional[Pointer[T, MutUntrackedOrigin]]:
    """An optional output slot when wanted, so the library skips that output otherwise."""
    if wanted:
        return slot
    return None


# endregion Signatures

# region Inputs


@fieldwise_init
struct BytesViewArguments(ImplicitlyCopyable, TrivialRegisterPassable):
    """`ustore_bytes_view_t`, laid out as the ABI declares it; register-passable, since the name tables
    take it by value."""

    var data: Address
    var length: c_size_t

    @staticmethod
    def empty() -> Self:
        """A zeroed view, which stands for the input's default."""
        return Self(NULL_ADDRESS, 0)


@fieldwise_init
struct BytesView[origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """A borrowed run of bytes and its length, never null-terminated: every path, name and filter.

    Parameters:
        origin: The borrowed storage, which the view keeps alive for as long as it is used.
    """

    var data: Optional[Pointer[UInt8, Self.origin]]
    """The first byte; absent for a zeroed view."""
    var length: Int
    """How many bytes the run covers, since nothing terminates it."""

    def __init__(out self):
        """A zeroed view, which stands for the input's default."""
        self.data = None
        self.length = 0

    @staticmethod
    def of[text_origin: ImmOrigin, //](text: StringSlice[text_origin]) -> BytesView[text_origin]:
        """Borrows a string's bytes, with no terminator manufactured."""
        return BytesView[text_origin](_borrowed[UInt8, text_origin](Int(text.unsafe_ptr())), text.byte_length())

    @staticmethod
    def of[bytes_origin: ImmOrigin, //](bytes: Span[UInt8, bytes_origin]) -> BytesView[bytes_origin]:
        """Borrows a run of raw bytes."""
        return BytesView[bytes_origin](_borrowed[UInt8, bytes_origin](Int(bytes.unsafe_ptr())), len(bytes))

    def arguments(self) -> BytesViewArguments:
        """The C layout, erased only inside a verb so the borrow lives through the call."""
        return BytesViewArguments(_address_of(self.data), c_size_t(self.length))


@fieldwise_init
struct StringsViewArguments(Defaultable, ImplicitlyCopyable):
    """`ustore_strings_view_t`, laid out as the ABI declares it, its union read as the two tape members."""

    var layout: c_int
    """`0` for 32-bit offsets, `1` for 64-bit offsets."""
    var data: Address
    var offsets: Address

    def __init__(out self):
        """A zeroed view, a u32 tape with null offsets, which stands for the input's default and is what a
        failed call leaves."""
        self = Self(c_int(0), NULL_ADDRESS, NULL_ADDRESS)


@fieldwise_init
struct StringsView[origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable, Sized):
    """A borrowed ragged batch of byte strings laid out as Apache Arrow lays one out: the bytes end to
    end, and one more offset than strings around them.

    Parameters:
        origin: The borrowed bytes and offsets, which the view keeps alive for as long as it is used.
    """

    var data: Optional[Pointer[UInt8, Self.origin]]
    """The first byte, which every offset counts from; absent for a zeroed view."""
    var offsets: Address
    """The strings' boundaries, one more of them than strings, as `layout` sizes them."""
    var layout: c_int
    """`0` for 32-bit offsets, `1` for 64-bit offsets."""
    var count: Int
    """Strings in the batch, or zero for a zeroed view."""

    def __init__(out self):
        """A zeroed view, which stands for the input's default."""
        self.data = None
        self.offsets = NULL_ADDRESS
        self.layout = c_int(0)
        self.count = 0

    def __len__(self) -> Int:
        """The strings in the batch."""
        return self.count

    def tasks(self) -> Int:
        """The tasks the batch can serve, one per string."""
        return self.count

    @staticmethod
    def of[
        bytes_origin: ImmOrigin, offsets_origin: ImmOrigin, //
    ](bytes: Span[UInt8, bytes_origin], offsets: Span[UInt32, offsets_origin]) -> StringsView[
        ImmOrigin(origin_of(_anchor[bytes_origin](), _anchor[offsets_origin]()))
    ]:
        """Borrows an Apache Arrow `Binary` tape: string `i` spans `offsets[i]` up to `offsets[i + 1]`."""
        comptime union = ImmOrigin(origin_of(_anchor[bytes_origin](), _anchor[offsets_origin]()))
        return StringsView[union](
            _borrowed[UInt8, union](Int(bytes.unsafe_ptr())),
            Int(offsets.unsafe_ptr()),
            c_int(0),
            max(len(offsets) - 1, 0),
        )

    @staticmethod
    def of[
        bytes_origin: ImmOrigin, offsets_origin: ImmOrigin, //
    ](bytes: Span[UInt8, bytes_origin], offsets: Span[UInt64, offsets_origin]) -> StringsView[
        ImmOrigin(origin_of(_anchor[bytes_origin](), _anchor[offsets_origin]()))
    ]:
        """Borrows an Apache Arrow `LargeBinary` tape, the same with 64-bit offsets."""
        comptime union = ImmOrigin(origin_of(_anchor[bytes_origin](), _anchor[offsets_origin]()))
        return StringsView[union](
            _borrowed[UInt8, union](Int(bytes.unsafe_ptr())),
            Int(offsets.unsafe_ptr()),
            c_int(1),
            max(len(offsets) - 1, 0),
        )

    def arguments(self) -> StringsViewArguments:
        """The C layout, erased only inside a verb so the borrow lives through the call."""
        return StringsViewArguments(self.layout, _address_of(self.data), self.offsets)


@fieldwise_init
struct PatternKind(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """What the strings of `Patterns` are matched as, `ustore_pattern_kind_t`."""

    var code: c_int
    """The raw `ustore_pattern_kind_t` value the ABI exchanges."""
    comptime REGEX = Self(0)
    """Regular expressions, of which a listed value matches at least one somewhere."""
    comptime SUBSTRINGS = Self(1)
    """Literal substrings, of which a listed value holds at least one."""


@fieldwise_init
struct CaseSensitivity(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Whether patterns compare bytes as they are or after Unicode case folding, `ustore_case_sensitivity_t`."""

    var code: c_int
    """The raw `ustore_case_sensitivity_t` value the ABI exchanges."""
    comptime CASED = Self(0)
    """Bytes as they are."""
    comptime UNCASED = Self(1)
    """Full Unicode case folding of both the patterns and the values."""


@fieldwise_init
struct PatternsArguments(Defaultable, ImplicitlyCopyable):
    """`ustore_patterns_t`, laid out as the ABI declares it."""

    var patterns: StringsViewArguments
    var count: c_size_t
    var kind: PatternKind
    var sensitivity: CaseSensitivity
    var weights: Address

    def __init__(out self):
        """Matches everything."""
        self = Self(StringsViewArguments(), 0, PatternKind.REGEX, CaseSensitivity.CASED, NULL_ADDRESS)


@fieldwise_init
struct Patterns[origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """Patterns a scan keeps the values or strings matching, compiled once per call or per cursor.

    Parameters:
        origin: The borrowed patterns and weights, kept alive for as long as these are used.
    """

    var patterns: StringsView[Self.origin]
    """The patterns; an empty batch matches everything."""
    var kind: PatternKind
    """What the patterns are matched as."""
    var sensitivity: CaseSensitivity
    """Whether matching folds case."""
    var weights: Optional[Pointer[Float32, Self.origin]]
    """One weight per substring pattern, which turns on scores; absent scores nothing."""

    def __init__(out self):
        """Matches everything."""
        self.patterns = StringsView[Self.origin]()
        self.kind = PatternKind.REGEX
        self.sensitivity = CaseSensitivity.CASED
        self.weights = None

    def __init__(
        out self,
        patterns: StringsView[Self.origin],
        kind: PatternKind = PatternKind.REGEX,
        sensitivity: CaseSensitivity = CaseSensitivity.CASED,
    ):
        """Matches `patterns` as `kind`, scoring nothing."""
        self.patterns = patterns
        self.kind = kind
        self.sensitivity = sensitivity
        self.weights = None

    def arguments(self) -> PatternsArguments:
        """The C layout, erased only inside a verb so the borrow lives through the call."""
        var weights = NULL_ADDRESS
        if case .Some(present) = self.weights:
            weights = Int(present)
        return PatternsArguments(
            self.patterns.arguments(), c_size_t(self.patterns.count), self.kind, self.sensitivity, weights
        )


# endregion Inputs

# region Outputs


def _in_arena[T: Copyable, origin: Origin](address: Address, length: Int) -> Span[T, origin]:
    """`length` values a call wrote into an arena; a failed or empty call left `address` null."""
    if length <= 0 or address == NULL_ADDRESS:
        return Span[T, origin]()
    return Span[T, origin](unsafe_ptr=Pointer[T, origin](unsafe_from_address=address), length=length)


def _bit(bits: Address, index: Int) -> Bool:
    """One bit of an Arrow bitmap the engine wrote, least significant first; a null bitmap sets none."""
    if bits == NULL_ADDRESS:
        return False
    var byte = Pointer[UInt8, ImmUntrackedOrigin](unsafe_from_address=bits + index // 8)[]
    return (byte >> UInt8(index % 8)) & 1 == 1


def _offset(view: StringsViewArguments, index: Int) -> Int:
    """One offset of a tape the engine wrote, in whichever width its layout names."""
    if view.layout == c_int(0):
        return Int(Pointer[UInt32, ImmUntrackedOrigin](unsafe_from_address=view.offsets + index * 4)[])
    return Int(Pointer[UInt64, ImmUntrackedOrigin](unsafe_from_address=view.offsets + index * 8)[])


@fieldwise_init
struct Tape[origin: MutOrigin](ImplicitlyCopyable, Sized):
    """A ragged run of byte strings a call wrote into the arena it borrows, read in place.

    Parameters:
        origin: The arena the strings live in, which this result borrows.
    """

    var arena: Pointer[Arena, Self.origin]
    """The arena holding the strings."""
    var generation: Int
    """The arena's call these strings came from, so a read after a later call is refused."""
    var view: StringsViewArguments
    """The bytes and offsets, as the engine wrote them."""
    var count: Int
    """Strings in the tape."""

    def __len__(self) -> Int:
        """The strings in the tape."""
        return self.count

    def __getitem__(ref self, index: Int) raises Error -> Span[UInt8, origin_of(self)]:
        """One string's bytes, borrowed from the arena.

        Raises:
            `Error` with `INVALID_ARGUMENT` once a later call rewound the arena, or `DEVICE` over device memory.
        """
        _readable(self.arena, self.generation, "Tape")
        if self.view.offsets == NULL_ADDRESS:
            return Span[UInt8, origin_of(self)]()
        var first = _offset(self.view, index)
        return _in_arena[UInt8, origin_of(self)](self.view.data + first, _offset(self.view, index + 1) - first)


# endregion Outputs

# region Placement


@fieldwise_init
struct RamOptionsArguments(ImplicitlyCopyable):
    """`ustore_ram_options_t`, laid out as the ABI declares it."""

    var memory_domains: Optional[Pointer[Int32, ImmUntrackedOrigin]]
    var memory_domains_count: c_size_t


@fieldwise_init
struct RamOptions[origin: ImmOrigin = ImmUntrackedOrigin](ImplicitlyCopyable):
    """The host memory domains a RAM executor serves, one NUMA node each.

    Parameters:
        origin: The borrowed node ID array, kept alive for as long as the options are used.
    """

    var memory_domains: Optional[Pointer[Int32, Self.origin]]
    """NUMA node IDs; absent derives every node the process may allocate on."""
    var memory_domains_count: Int
    """Entries in `memory_domains`; zero derives."""

    def __init__(out self):
        """Derives every memory domain the process may allocate on."""
        self.memory_domains = None
        self.memory_domains_count = 0

    @staticmethod
    def on[domains_origin: ImmOrigin, //](memory_domains: Span[Int32, domains_origin]) -> RamOptions[domains_origin]:
        """Names the NUMA nodes explicitly, borrowing the caller's array for the call."""
        return RamOptions[domains_origin](
            Pointer[Int32, domains_origin](unsafe_from_address=Int(memory_domains.unsafe_ptr())), len(memory_domains)
        )

    def arguments(self) -> RamOptionsArguments:
        """The C layout, erased only inside a verb so the borrow lives through the call."""
        var domains: Optional[Pointer[Int32, ImmUntrackedOrigin]] = None
        if case .Some(named) = self.memory_domains:
            domains = named.unsafe_origin_cast[ImmUntrackedOrigin]()
        return RamOptionsArguments(domains, c_size_t(self.memory_domains_count))


@fieldwise_init
struct NvmeOptions(Defaultable, ImplicitlyCopyable):
    """How the threads of an NVMe executor move bytes, `ustore_nvme_options_t`; every zero keeps the default."""

    var transport: Transport
    """How bytes move between the files and memory."""
    var shared_io_channels: c_size_t
    """I/O channels the threads outside the executor share."""
    var queue_depth: c_size_t
    """Submissions a channel holds at once."""
    var request_buffer_bytes: c_size_t
    """Bytes of each request buffer of a channel."""
    var request_buffers_count: c_size_t
    """Request buffers of each channel."""
    var stream_buffer_bytes: c_size_t
    """Bytes of each streaming buffer of a channel."""
    var stream_buffers_count: c_size_t
    """Streaming buffers of each channel."""

    def __init__(out self):
        """Every engine default."""
        self = Self(Transport.AUTOMATIC, 0, 0, 0, 0, 0, 0)


# endregion Placement

# region Executors


@fieldwise_init
struct GPUDevicesArguments(ImplicitlyCopyable):
    """`ustore_gpu_devices_t`, laid out as the ABI declares it."""

    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var devices: Optional[Pointer[GPUDevice, MutUntrackedOrigin]]
    var devices_capacity: c_size_t
    var devices_count: Optional[Pointer[c_size_t, MutUntrackedOrigin]]


@fieldwise_init
struct ExecutorForRamArguments(ImplicitlyCopyable):
    """`ustore_executor_for_ram_t`, laid out as the ABI declares it."""

    var executor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var ram: RamOptionsArguments
    var threads: c_size_t
    var deferred_limit: c_size_t
    var cpu_class: CPUClass


@fieldwise_init
struct ExecutorOnPoolArguments(ImplicitlyCopyable):
    """`ustore_executor_on_pool_t`, laid out as the ABI declares it."""

    var executor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var ram: RamOptionsArguments
    var pool: Address
    var deferred_limit: c_size_t


@fieldwise_init
struct ExecutorForNvmeArguments(ImplicitlyCopyable):
    """`ustore_executor_for_nvme_t`, laid out as the ABI declares it."""

    var executor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var nvme: NvmeOptions
    var threads: c_size_t
    var deferred_limit: c_size_t
    var cpu_class: CPUClass


@fieldwise_init
struct ExecutorForVramArguments(ImplicitlyCopyable):
    """`ustore_executor_for_vram_t`, laid out as the ABI declares it."""

    var executor: Optional[Pointer[Address, MutUntrackedOrigin]]
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var device: Int32
    var stream: Address
    var scratch_limit: c_size_t
    var gds: Gds
    var deferred_limit: c_size_t


@fieldwise_init
struct ExecutorDrainArguments(ImplicitlyCopyable):
    """`ustore_executor_drain_t`, laid out as the ABI declares it."""

    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var mood: Mood


struct _ExecutorHandle:
    """The `ustore_executor_t` an `Executor` owns, shared with every device arena it made and freed with
    the last of them."""

    var library: Library
    var address: Address

    def __init__(out self, library: Library, address: Address):
        """Takes over a spawned handle, or the null one the caller-thread executor names."""
        self.library = library
        self.address = address

    def __deinit__(deinit self):
        """Drains every call, then frees the executor. Calls `ustore_executor_free`.

        The loader reference is surrendered only after the call: reading the entry point is the
        last use of `library`, so without the trailing move Mojo would `dlclose` the engine between
        the load and the branch to it.
        """
        self.library.symbols().executor_free(self.address)
        _ = self.library^


def _created(
    library: Library, outcome: Pointer[Outcome, MutUntrackedOrigin], created: Pointer[Address, MutUntrackedOrigin], detail: StaticString
) raises Error -> Address:
    """The handle a creating verb wrote, or its failure raised."""
    library._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], detail)
    return created.unsafe_origin_cast[MutAnyOrigin]()[]


struct Executor:
    """The threads or rings a verb runs on, freed once it and every device arena it made go out of scope.

    `Executor(library)` owns nothing: every verb handed it runs on the calling thread. Calls on one
    executor start in submission order, and a blocking call runs alone.
    """

    var library: Library
    """The loaded engine, held so its symbol table outlives every call made through the handle."""
    var address: Address
    """The opaque `ustore_executor_t` every verb hands back to the engine; `NULL_ADDRESS` runs inline."""
    var vram: Bool
    """Whether it drives a GPU, so the arenas it makes hold device memory."""
    var _handle: ArcPointer[_ExecutorHandle]
    """The handle's owner, which a device arena shares so the executor outlives it."""

    def __init__(out self, library: Library):
        """The caller-thread executor, which every verb accepts.

        Args:
            library: The loaded engine, kept so the shared object stays mapped.
        """
        self = Self(library=library, address=NULL_ADDRESS, vram=False)

    def __init__(out self, *, library: Library, address: Address, vram: Bool):
        """Adopts a handle that is already spawned, which is how the factories hand one back."""
        self.library = library
        self.address = address
        self.vram = vram
        self._handle = ArcPointer(_ExecutorHandle(library, address))

    @staticmethod
    def for_ram[
        ram_origin: ImmOrigin, //
    ](
        library: Library,
        ram: RamOptions[ram_origin] = RamOptions(),
        threads: Int = 0,
        deferred_limit: Int = 0,
        cpu_class: CPUClass = CPUClass.UNKNOWN,
    ) raises Error -> Self:
        """Spawns host threads for the RAM, RocksDB and LevelDB engines. Calls `ustore_executor_for_ram`.

        Args:
            library: The loaded engine, kept so the shared object stays mapped.
            ram: The memory domains it serves; copied, not borrowed.
            threads: Threads to spawn; zero takes 7/8 of the cores those domains carry.
            deferred_limit: Deferred calls it holds at once; zero takes a default.
            cpu_class: The class its threads confine their waits and atomics to; `UNKNOWN` derives.

        Raises:
            `Error` if the domains cannot be resolved or the OS refuses the threads.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            ExecutorForRamArguments(created, outcome, ram.arguments(), c_size_t(threads), c_size_t(deferred_limit), cpu_class),
            library.symbols().executor_for_ram,
        )
        return Self(library=library, address=_created(library, outcome, created, "ustore_executor_for_ram"), vram=False)

    @staticmethod
    def on_pool[
        ram_origin: ImmOrigin, //
    ](library: Library, pool: Address, ram: RamOptions[ram_origin] = RamOptions(), deferred_limit: Int = 0) raises Error -> Self:
        """Schedules onto a borrowed ForkUnion pool, so several libraries in one process share one set of
        threads. Calls `ustore_executor_on_pool`.

        Args:
            library: The loaded engine, kept so the shared object stays mapped.
            pool: A live ForkUnion `fu_pool_t`, which must outlive the executor.
            ram: The memory domains it serves; copied, not borrowed.
            deferred_limit: Deferred calls it holds at once; zero takes a default.

        Raises:
            `Error` if the pool or the domains are refused.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            ExecutorOnPoolArguments(created, outcome, ram.arguments(), pool, c_size_t(deferred_limit)),
            library.symbols().executor_on_pool,
        )
        return Self(library=library, address=_created(library, outcome, created, "ustore_executor_on_pool"), vram=False)

    @staticmethod
    def for_nvme(
        library: Library,
        nvme: NvmeOptions = NvmeOptions(),
        threads: Int = 0,
        deferred_limit: Int = 0,
        cpu_class: CPUClass = CPUClass.UNKNOWN,
    ) raises Error -> Self:
        """Spawns threads for the NVMe engine, each submitting and polling its own I/O ring.
        Calls `ustore_executor_for_nvme`.

        Args:
            library: The loaded engine, kept so the shared object stays mapped.
            nvme: How the databases it opens move bytes; copied, not borrowed.
            threads: Threads to spawn; zero derives them from the cores.
            deferred_limit: Deferred calls it holds at once; zero takes a default.
            cpu_class: The class its threads confine their waits and atomics to; `UNKNOWN` derives.

        Raises:
            `Error` if the transport is unknown or the OS refuses the threads.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            ExecutorForNvmeArguments(created, outcome, nvme, c_size_t(threads), c_size_t(deferred_limit), cpu_class),
            library.symbols().executor_for_nvme,
        )
        return Self(library=library, address=_created(library, outcome, created, "ustore_executor_for_nvme"), vram=False)

    @staticmethod
    def for_vram(
        library: Library,
        device: Int = 0,
        stream: Address = NULL_ADDRESS,
        scratch_limit: Int = 0,
        gds: Gds = Gds.AUTOMATIC,
        deferred_limit: Int = 0,
    ) raises Error -> Self:
        """Opens one submission stream on `device`, or borrows the caller's, driven by one host thread.
        Calls `ustore_executor_for_vram`.

        Args:
            library: The loaded engine, kept so the shared object stays mapped.
            device: The UStore ordinal of the device, its position in `Library.gpu_devices`.
            stream: A borrowed stream on that device, which must outlive the executor; null creates one.
            scratch_limit: Ceiling on the device scratch a call takes; zero lets it grow.
            gds: How reads and compactions move bytes between the drives and the device.
            deferred_limit: Deferred calls it holds at once; zero takes a default.

        Raises:
            `Error` with `DEVICE` if no device carries the ordinal.
        """
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(
            ExecutorForVramArguments(
                created, outcome, Int32(device), stream, c_size_t(scratch_limit), gds, c_size_t(deferred_limit)
            ),
            library.symbols().executor_for_vram,
        )
        return Self(library=library, address=_created(library, outcome, created, "ustore_executor_for_vram"), vram=True)

    def drain(self, mood: Mood = Mood.PARKED) raises Error:
        """Waits for every call submitted before the drain, then adopts `mood` whenever it next idles.
        Calls `ustore_executor_drain`.

        Raises:
            `Error` for a failure of the executor itself, never one of a drained call.
        """
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(ExecutorDrainArguments(self.address, outcome, mood), self.library.symbols().executor_drain)
        self.library._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], "ustore_executor_drain")


# endregion Executors

# region Arenas


@fieldwise_init
struct ArenaInitArguments(ImplicitlyCopyable):
    """`ustore_arena_init_t`, laid out as the ABI declares it."""

    var arena: Optional[Pointer[Address, MutUntrackedOrigin]]
    var executor: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]


@fieldwise_init
struct Residency(Equatable, ImplicitlyCopyable, TrivialRegisterPassable):
    """Where an arena's outputs live, `ustore_residency_t`."""

    var code: c_int
    """The raw `ustore_residency_t` value the ABI exchanges."""
    comptime UNKNOWN = Self(0)
    """No residency, which only a zeroed description reads as."""
    comptime RAM = Self(1)
    """Host pages, which the calling thread reads in place."""
    comptime VRAM = Self(2)
    """One device's memory, which only that device's code reads."""


@fieldwise_init
struct ArenaDescription(Defaultable, ImplicitlyCopyable):
    """Where an arena's outputs live, `ustore_arena_description_t`."""

    var residency: Residency
    """RAM for host pages, VRAM for one device's memory."""
    var device: Int32
    """The ordinal of the device, as `Library.gpu_devices` lists it, or -1 for host pages."""

    def __init__(out self):
        """Zeroes both fields, which is what a failed report leaves."""
        self = Self(Residency.UNKNOWN, 0)


@fieldwise_init
struct ArenaDescribeArguments(ImplicitlyCopyable):
    """`ustore_arena_describe_t`, laid out as the ABI declares it."""

    var arena: Address
    var outcome: Optional[Pointer[Outcome, MutUntrackedOrigin]]
    var description: Optional[Pointer[ArenaDescription, MutUntrackedOrigin]]


struct Arena:
    """The memory every output whose length scales with a call lands in, freed when it goes out of scope.

    Made where an executor computes, decided once; nothing is allocated until a call writes into it.
    Every call handed it rewinds and refills it, which invalidates the previous call's outputs, so a
    steady loop allocates once. One arena serves one call at a time; a second concurrent use is
    refused by the engine as `CONTENDED`, which leaves the holder's results readable.

    A result refuses every read with `INVALID_ARGUMENT` once a later call rewinds its arena, and a
    result over a device arena refuses every host read with `DEVICE`.
    """

    var _library: Library
    """The loaded engine, held so the release can still reach `ustore_arena_free`."""
    var _address: Address
    """The `ustore_arena_t`, never null."""
    var _vram: Bool
    """Whether its memory is a GPU's, so a host read of it is refused rather than made."""
    var _executor: Optional[ArcPointer[_ExecutorHandle]]
    """The device executor that made it, held so the arena is freed first."""
    var _generation: Int
    """Calls the engine took on it so far, which every result records to know when a later one rewound it."""

    def __init__(out self, library: Library) raises Error:
        """Host pages for calls on the calling thread. Calls `ustore_arena_init`."""
        self = Self(Executor(library))

    def __init__(out self, executor: Executor) raises Error:
        """Memory where `executor` computes, serving the calls of its kind. Calls `ustore_arena_init`."""
        var created = stack_allocation[1, Address]().unsafe_origin_cast[MutUntrackedOrigin]()
        created[] = NULL_ADDRESS
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        _cross(ArenaInitArguments(created, executor.address, outcome), executor.library.symbols().arena_init)
        self._library = executor.library
        self._address = _created(executor.library, outcome, created, "ustore_arena_init")
        self._vram = executor.vram
        self._executor = Optional(executor._handle) if executor.vram else None
        self._generation = 0

    def __deinit__(deinit self):
        """Frees the arena and every output in it, then lets a device arena's executor go.
        Calls `ustore_arena_free`.

        The executor and the loader reference are surrendered only after the call, for the reason
        `_ExecutorHandle` records.
        """
        self._library.symbols().arena_free(self._address)
        _ = self._executor^
        _ = self._library^

    def vram(self) -> Bool:
        """Whether its memory is a GPU's, which host reads refuse. Binding-only."""
        return self._vram

    def describe(self) raises Error -> ArenaDescription:
        """Where the arena's outputs live, as the library reports it. Calls `ustore_arena_describe`."""
        var outcome = stack_allocation[1, Outcome]().unsafe_origin_cast[MutUntrackedOrigin]()
        outcome[] = Outcome()
        var description = stack_allocation[1, ArenaDescription]().unsafe_origin_cast[MutUntrackedOrigin]()
        description[] = ArenaDescription()
        _cross(ArenaDescribeArguments(self._address, outcome, description), self._library.symbols().arena_describe)
        self._library._check(outcome.unsafe_origin_cast[MutAnyOrigin]()[], "ustore_arena_describe")
        return description.unsafe_origin_cast[MutAnyOrigin]()[]


def _served[origin: MutOrigin](arena: Pointer[Arena, origin], status: Status) -> Int:
    """Retires every earlier result once the engine took a call naming the arena; a `CONTENDED` refusal
    never touched it. Answers the generation the call's result is stamped with."""
    var served = Pointer[Arena, MutAnyOrigin](unsafe_from_address=Int(arena))
    if status != Status.CONTENDED:
        served[]._generation += 1
    return served[]._generation


def _readable[origin: MutOrigin](arena: Pointer[Arena, origin], generation: Int, detail: StaticString) raises Error:
    """Refuses a host read of a result over device memory, or over an arena a later call rewound."""
    if arena[]._vram:
        raise Error(Status.DEVICE, detail)
    if arena[]._generation != generation:
        raise Error(Status.INVALID_ARGUMENT, detail)


# endregion Arenas

# region Calls


struct Unwritten(Answer, Defaultable, ImplicitlyCopyable):
    """The outputs of a verb that writes nothing but its outcome, and its result."""

    comptime Written = Unwritten

    def __init__(out self):
        """Holds nothing."""
        pass

    def __init__(out self, arena: Address, generation: Int, written: Unwritten, count: Int):
        """Holds nothing."""
        pass


trait Answer(Movable, Deinitable):
    """What a verb's caller reads once the call is final, built from what the engine wrote."""

    comptime Written: ImplicitlyCopyable & Defaultable & Deinitable
    """The fixed-size outputs the engine writes through, such as views into the arena."""

    def __init__(out self, arena: Address, generation: Int, written: Self.Written, count: Int):
        """Adopts what the call wrote into `arena`, stamped `generation`, for `count` tasks."""
        ...


trait Arguments(ImplicitlyCopyable, Deinitable):
    """One verb's argument structure, laid out as the ABI declares it, with its inputs already set."""

    comptime Written: ImplicitlyCopyable & Defaultable & Deinitable
    """The fixed-size outputs the engine writes through."""
    comptime symbol: StaticString
    """The C entry point, carried into every error."""

    def bind(
        mut self,
        scope: Scope,
        completion: Completion,
        outcome: Pointer[Outcome, MutUntrackedOrigin],
        written: Pointer[Self.Written, MutUntrackedOrigin],
        arena: Address,
    ):
        """Sets the handle, executor, outcome, completion, output slots and arena of the receiver's call."""
        ...

    @staticmethod
    def verb(library: Library) -> Verb[Self]:
        """The resolved entry point."""
        ...


@fieldwise_init
struct CallSlots[Outputs: ImplicitlyCopyable & Defaultable & Deinitable](ImplicitlyCopyable):
    """The outcome and the fixed-size outputs one call writes through, at one stable address."""

    var outcome: Outcome
    var outputs: Self.Outputs

    def __init__(out self):
        """Zeroes the outcome and defaults every output."""
        self.outcome = Outcome()
        self.outputs = Self.Outputs()


trait Call(Deinitable, Movable):
    """One verb crossing into the engine: what it lends until final, and how its outputs become a result.

    Each receiver routes the same `Call`: a handle and `handle.on(executor)` block on it, and
    `handle.deferred(executor)` wraps it in a `Deferred`, which is what keeps the verbs symmetric.
    """

    comptime Outputs: ImplicitlyCopyable & Defaultable & Deinitable
    """The fixed-size outputs the engine writes through."""
    comptime Result: Movable & Deinitable
    """What a successful call answers."""
    comptime symbol: StaticString
    """The C entry point, carried into every error."""

    def submit(
        mut self,
        scope: Scope,
        completion: Completion,
        outcome: Pointer[Outcome, MutUntrackedOrigin],
        outputs: Pointer[Self.Outputs, MutUntrackedOrigin],
    ):
        """Crosses into the engine."""
        ...

    def served(self, status: Status) -> Int:
        """Stamps the call's arena once the engine took the call, and answers the stamp."""
        ...

    def conclude(deinit self, outputs: Self.Outputs, generation: Int) -> Self.Result:
        """Adopts what the final call wrote, success or not, and builds the result around its arena."""
        ...


struct _Crossing[A: Arguments, R: Answer, origin: MutOrigin, inputs: ImmOrigin](Call):
    """The crossing of one verb: its argument structure, the arena its outputs land in, and how many tasks
    it carries. `inputs` is the union of every borrowed input, kept alive with the call."""

    comptime Outputs = Self.A.Written
    comptime Result = Self.R
    comptime symbol: StaticString = Self.A.symbol
    var arguments: Self.A
    var arena: Optional[Pointer[Arena, Self.origin]]
    var count: Int

    def __init__(out self, arguments: Self.A, arena: Optional[Pointer[Arena, Self.origin]], count: Int):
        """Wraps a verb's arguments with the arena it rewinds, if any."""
        self.arguments = arguments
        self.arena = arena
        self.count = count

    def submit(
        mut self,
        scope: Scope,
        completion: Completion,
        outcome: Pointer[Outcome, MutUntrackedOrigin],
        outputs: Pointer[Self.A.Written, MutUntrackedOrigin],
    ):
        var arena = NULL_ADDRESS
        if case .Some(lent) = self.arena:
            arena = lent[]._address
        self.arguments.bind(scope, completion, outcome, outputs, arena)
        _cross(self.arguments, Self.A.verb(scope.library))

    def served(self, status: Status) -> Int:
        if case .Some(lent) = self.arena:
            return _served(lent, status)
        return 0

    def conclude(deinit self, outputs: Self.A.Written, generation: Int) -> Self.R:
        var arena = NULL_ADDRESS
        if case .Some(lent) = self.arena:
            arena = Int(lent)
        return Self.R(arena, generation, rebind[Self.R.Written](outputs), self.count)


def _block[C: Call](scope: Scope, var call: C) raises Error -> C.Result:
    """Runs one call to its final status, its slots on the stack, and answers its result or raises."""
    var slots = stack_allocation[1, CallSlots[C.Outputs]]()
    slots[unsafe_offset=0] = CallSlots[C.Outputs]()
    var mutable_slots = slots.unsafe_origin_cast[MutAnyOrigin]()
    call.submit(
        scope,
        Completion.BLOCKING,
        Pointer(to=mutable_slots[unsafe_offset=0].outcome).unsafe_origin_cast[MutUntrackedOrigin](),
        Pointer(to=mutable_slots[unsafe_offset=0].outputs).unsafe_origin_cast[MutUntrackedOrigin](),
    )
    var written = mutable_slots[unsafe_offset=0]
    var generation = call.served(Status.of(written.outcome.status))
    var result = call^.conclude(written.outputs, generation)
    scope.library._check(written.outcome, C.symbol)
    return result^


struct Deferred[C: Call](Movable):
    """A call submitted to an executor, which owns its outcome and outputs until it is final. Binding-only.

    `status` reads `DEFERRED` until then, without blocking, and `wait` blocks until the call is
    final and answers its result or raises its failure, as many times as it is asked. The result is
    the view a blocking call answers, borrowing the same arena. Dropping an unfinished one waits the
    same way, so nothing the engine still writes is ever freed under it. The crossing's origins keep
    the arena and every borrowed input alive for as long as this is, and freeing the database, the
    executor or a transaction waits for the calls naming them.

    Parameters:
        C: The verb's crossing, which names the result.
    """

    var library: Library
    """The loaded engine, held so the wait can still reach `ustore_outcome_wait`."""
    var slots: OwnedPointer[CallSlots[Self.C.Outputs]]
    """The outcome and outputs the engine writes, on the heap since they outlive the verb."""
    var call: Optional[Self.C]
    """What the call lends the engine, until a wait concludes it."""
    var answer: Optional[Self.C.Result]
    """The result, once a wait found the call succeeded."""
    var failure: Optional[Error]
    """The failure, once a wait found the call failed."""
    var generation: Int
    """The stamp `served` answered at submission, so a result waited on late still reads as stale."""

    def __init__(out self, scope: Scope, var call: Self.C):
        """Submits `call` deferred, on the receiver's executor."""
        self.library = scope.library
        self.slots = OwnedPointer(CallSlots[Self.C.Outputs]())
        self.call = call^
        self.answer = None
        self.failure = None
        self.generation = 0
        var slots = self._slots()
        self.call.value().submit(
            scope,
            Completion.DEFERRED,
            Pointer(to=slots[].outcome).unsafe_origin_cast[MutUntrackedOrigin](),
            Pointer(to=slots[].outputs).unsafe_origin_cast[MutUntrackedOrigin](),
        )
        self.generation = self.call.value().served(self.status())

    def __deinit__(deinit self):
        """Waits until the call is final, then frees what it lent. Calls `ustore_outcome_wait`."""
        if self.call:
            var slots = Pointer[CallSlots[Self.C.Outputs], MutAnyOrigin](
                unsafe_from_address=Int(Pointer(to=self.slots[]))
            )
            _ = self.library.symbols().outcome_wait(Pointer(to=slots[].outcome))
            _ = self.call.take().conclude(slots[].outputs, self.generation)
        _ = self.library^

    def _slots(self) -> Pointer[CallSlots[Self.C.Outputs], MutAnyOrigin]:
        """The heap slots, through the origin the engine's writes need."""
        return Pointer[CallSlots[Self.C.Outputs], MutAnyOrigin](unsafe_from_address=Int(Pointer(to=self.slots[])))

    def status(self) -> Status:
        """`DEFERRED` until the call is final, then its final status; never blocks. Calls `ustore_outcome_status`."""
        return Status(self.library.symbols().outcome_status(Pointer(to=self._slots()[].outcome)))

    def wait(mut self) raises Error -> ref[self] Self.C.Result:
        """Blocks until the call is final, then answers its result or raises its failure, every time it
        is asked. Calls `ustore_outcome_wait`.

        Raises:
            `Error` carrying the call's failure.
        """
        if self.call:
            var slots = self._slots()
            _ = self.library.symbols().outcome_wait(Pointer(to=slots[].outcome))
            var written = slots[]
            var result = self.call.take().conclude(written.outputs, self.generation)
            if written.outcome.status == c_int(0):
                self.answer = result^
            else:
                self.failure = self.library._error(written.outcome, Self.C.symbol)
        if self.failure:
            raise self.failure.value()
        return Pointer(to=self.answer.value()).unsafe_origin_cast[origin_of(self)]()[]


# endregion Calls
