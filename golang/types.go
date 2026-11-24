// Shared vocabulary of every UStore header: statuses, enums, strings, executors, arenas and calls.
//
// File: golang/types.go
// Author: Ash Vardanian
// Date: November 18, 2026

package ustore

import (
	"fmt"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

/*
#cgo CFLAGS: -I${SRCDIR}/../c
#cgo LDFLAGS: -lustore_static
#cgo linux LDFLAGS: -lstdc++ -lm -lpthread
#cgo darwin LDFLAGS: -lc++
#cgo freebsd LDFLAGS: -lc++ -lm -lpthread
#cgo windows LDFLAGS: -lstdc++ -lbcrypt

// A function is noescape when C keeps none of its Go pointers past the call, and nocallback when
// its argument structure carries no callback field: an executor runs only the callbacks of the call
// that names them.
#cgo noescape ustore_outcome_status
#cgo nocallback ustore_outcome_status
#cgo noescape ustore_outcome_wait
#cgo nocallback ustore_outcome_wait
#cgo noescape ustore_executor_for_ram
#cgo nocallback ustore_executor_for_ram
#cgo noescape ustore_executor_for_nvme
#cgo nocallback ustore_executor_for_nvme
#cgo noescape ustore_executor_for_vram
#cgo nocallback ustore_executor_for_vram
#cgo noescape ustore_executor_drain
#cgo nocallback ustore_executor_drain
#cgo noescape ustore_gpu_devices
#cgo nocallback ustore_gpu_devices
#cgo noescape ustore_arena_init
#cgo nocallback ustore_arena_init
#cgo noescape ustore_arena_describe
#cgo nocallback ustore_arena_describe
#cgo noescape ustore_version
#cgo nocallback ustore_version
#cgo noescape ustore_status_from_name
#cgo nocallback ustore_status_from_name
#cgo noescape ustore_phase_from_name
#cgo nocallback ustore_phase_from_name
#cgo noescape ustore_pattern_kind_from_name
#cgo nocallback ustore_pattern_kind_from_name
#cgo noescape ustore_case_sensitivity_from_name
#cgo nocallback ustore_case_sensitivity_from_name
#cgo noescape ustore_mood_from_name
#cgo nocallback ustore_mood_from_name
#cgo noescape ustore_cpu_class_from_name
#cgo nocallback ustore_cpu_class_from_name
#cgo noescape ustore_transport_from_name
#cgo nocallback ustore_transport_from_name
#cgo noescape ustore_gds_from_name
#cgo nocallback ustore_gds_from_name
#cgo noescape ustore_gpu_vendor_from_name
#cgo nocallback ustore_gpu_vendor_from_name
#cgo noescape ustore_gpu_class_from_name
#cgo nocallback ustore_gpu_class_from_name

#include "shim.h"
*/
import "C"

// region: Errors

// Status names what went wrong, in a vocabulary shared by every engine and by this binding.
// Validation performed here before a call crosses reports the status the engine would have
// reported for the same fault.
//
// A Status is an error, so errors.Is matches any failure carrying it against a bare Status.
type Status int32

// The statuses a call ends in. Only [Deferred.Status] reports StatusDeferred, the one status that
// is not final.
const (
	// StatusSuccess means the call succeeded and carries no message.
	StatusSuccess Status = C.ustore_status_success_k
	// StatusDeferred means the call was submitted deferred and is not final yet.
	StatusDeferred Status = C.ustore_status_deferred_k
	// StatusOutOfMemory means an allocation could not be satisfied.
	StatusOutOfMemory Status = C.ustore_status_out_of_memory_k
	// StatusReadOnly means the call would change a database opened read-only.
	StatusReadOnly Status = C.ustore_status_read_only_k
	// StatusIO means a file or directory could not be opened, read, written, or closed.
	StatusIO Status = C.ustore_status_io_k
	// StatusDevice means a device could not be selected or its memory claimed, or the call's executor,
	// database and arena belong to different residencies or devices.
	StatusDevice Status = C.ustore_status_device_k
	// StatusMalformed means stored or supplied data could not be decoded.
	StatusMalformed Status = C.ustore_status_malformed_k
	// StatusInternal means the engine failed in a way no other status names.
	StatusInternal Status = C.ustore_status_internal_k
	// StatusUninitialized means the call needs a handle that was never opened or is closed.
	StatusUninitialized Status = C.ustore_status_uninitialized_k
	// StatusContended means the call was refused rather than waited for: another call holds its
	// executor, arena, cursor or transaction.
	StatusContended Status = C.ustore_status_contended_k
	// StatusCancelled means a progress handler stopped the call, which unwound and published nothing.
	StatusCancelled Status = C.ustore_status_cancelled_k
	// StatusUnsupported means the engine or build does not implement what the call asks for.
	StatusUnsupported Status = C.ustore_status_unsupported_k
	// StatusInvalidArgument means an argument is missing, contradicts another one, or carries a choice
	// the call does not accept.
	StatusInvalidArgument Status = C.ustore_status_invalid_argument_k
	// StatusNotFound means a collection, a directory, or a document a modification needs does not
	// exist.
	StatusNotFound Status = C.ustore_status_not_found_k
	// StatusAlreadyExists means a collection name or a document the call creates is already taken.
	StatusAlreadyExists Status = C.ustore_status_already_exists_k
	// StatusWriteConflict means a concurrent writer changed a key this transaction wrote.
	StatusWriteConflict Status = C.ustore_status_write_conflict_k
	// StatusReadConflict means a concurrent writer changed a key this transaction read.
	StatusReadConflict Status = C.ustore_status_read_conflict_k
	// StatusPhantomConflict means a concurrent writer inserted into a range this transaction scanned.
	StatusPhantomConflict Status = C.ustore_status_phantom_conflict_k
	// StatusCapacityExhausted means a hard design ceiling was reached.
	StatusCapacityExhausted Status = C.ustore_status_capacity_exhausted_k
)

// String renders the status as the library spells it, such as "not-found".
func (status Status) String() string {
	return C.GoString(C.ustore_status_name(C.ustore_status_t(status)))
}

// StatusFromName reads a status back from the spelling [Status.String] renders.
func StatusFromName(name string) (Status, error) {
	var status C.ustore_status_t
	err := named(C.ustore_status_from_name(view(name), &status))
	return Status(status), err
}

// Error implements the error interface, so a bare Status is an errors.Is target.
func (status Status) Error() string { return status.String() }

// Error is one failed call: a status every caller can branch on, and the detail the status
// cannot carry. Reach the status with errors.Is or errors.As.
type Error struct {
	Status  Status
	Message string
}

// Error implements the error interface.
func (failure *Error) Error() string {
	return fmt.Sprintf("ustore: %s: %s", failure.Status, failure.Message)
}

// Unwrap exposes the status, which is what errors.Is compares.
func (failure *Error) Unwrap() error { return failure.Status }

// fail builds the failure this binding reports for a fault it caught before the boundary.
func fail(status Status, message string) *Error {
	return &Error{Status: status, Message: message}
}

// check turns a final outcome into a Go error. Messages are static library storage and need no
// deallocation, so the copy is the only cost.
func check(outcome C.ustore_outcome_t) error {
	if outcome.status == C.ustore_status_success_k {
		return nil
	}
	return &Error{Status: Status(outcome.status), Message: C.GoString(outcome.message)}
}

// endregion: Errors

// region: Enums

// named turns the status a spelling lookup answers into a Go error.
func named(status C.ustore_status_t) error {
	if status == C.ustore_status_success_k {
		return nil
	}
	return fail(Status(status), "the spelling names no value of its enum")
}

// Phase is the kind of work a plan is doing, shared by every long operation.
type Phase int32

const (
	// PhaseNone means no plan, or one that has not started.
	PhaseNone Phase = C.ustore_phase_none_k
	// PhaseFlush writes what memory holds to storage.
	PhaseFlush Phase = C.ustore_phase_flush_k
	// PhaseMerge merges or vacuums the stored versions one unit at a time.
	PhaseMerge Phase = C.ustore_phase_merge_k
	// PhaseReclaim frees what no reader reaches any more.
	PhaseReclaim Phase = C.ustore_phase_reclaim_k
	// PhaseDone means the plan ran to its end, and nothing is pending.
	PhaseDone Phase = C.ustore_phase_done_k
)

// String renders the phase as the library spells it.
func (phase Phase) String() string { return C.GoString(C.ustore_phase_name(C.ustore_phase_t(phase))) }

// PhaseFromName reads a phase back from the spelling [Phase.String] renders.
func PhaseFromName(name string) (Phase, error) {
	var phase C.ustore_phase_t
	err := named(C.ustore_phase_from_name(view(name), &phase))
	return Phase(phase), err
}

// PhaseProgress is where a plan stands: its phase, that phase's position in the plan, and its own
// count, laid out as ustore_phase_progress_t.
type PhaseProgress struct {
	// Phase is the kind of work in hand.
	Phase Phase
	// Ordinal is this phase's position among the ones the plan runs, from zero.
	Ordinal uint64
	// Completed counts the units of this phase done.
	Completed uint64
	// Total counts the units of this phase in all.
	Total uint64
}

// The library writes the progress of a plan in place, so the two layouts must agree.
var _ = [1]struct{}{}[unsafe.Sizeof(PhaseProgress{})-unsafe.Sizeof(C.ustore_phase_progress_t{})]

// PatternKind is what the strings of [Patterns] are matched as.
type PatternKind int32

const (
	// PatternRegex matches regular expressions, of which a value matches at least one somewhere.
	PatternRegex PatternKind = C.ustore_pattern_regex_k
	// PatternSubstrings matches literal substrings, found together through Aho-Corasick.
	PatternSubstrings PatternKind = C.ustore_pattern_substrings_k
)

// String renders the pattern kind as the library spells it.
func (kind PatternKind) String() string {
	return C.GoString(C.ustore_pattern_kind_name(C.ustore_pattern_kind_t(kind)))
}

// PatternKindFromName reads a pattern kind back from the spelling [PatternKind.String] renders.
func PatternKindFromName(name string) (PatternKind, error) {
	var kind C.ustore_pattern_kind_t
	err := named(C.ustore_pattern_kind_from_name(view(name), &kind))
	return PatternKind(kind), err
}

// CaseSensitivity is whether patterns compare bytes as they are or after Unicode case folding.
type CaseSensitivity int32

const (
	// Cased compares bytes as they are.
	Cased CaseSensitivity = C.ustore_cased_k
	// Uncased folds the case of both the patterns and the values.
	Uncased CaseSensitivity = C.ustore_uncased_k
)

// String renders the case sensitivity as the library spells it.
func (sensitivity CaseSensitivity) String() string {
	return C.GoString(C.ustore_case_sensitivity_name(C.ustore_case_sensitivity_t(sensitivity)))
}

// CaseSensitivityFromName reads a case sensitivity back from the spelling [CaseSensitivity.String]
// renders.
func CaseSensitivityFromName(name string) (CaseSensitivity, error) {
	var sensitivity C.ustore_case_sensitivity_t
	err := named(C.ustore_case_sensitivity_from_name(view(name), &sensitivity))
	return CaseSensitivity(sensitivity), err
}

// Mood is what an executor's threads do between a drain and its next call.
type Mood int32

const (
	// MoodParked sleeps in the kernel until the next call, freeing the cores.
	MoodParked Mood = C.ustore_mood_parked_k
	// MoodSpinning keeps polling, trading the cores for the lowest wake-up latency.
	MoodSpinning Mood = C.ustore_mood_spinning_k
)

// String renders the mood as the library spells it.
func (mood Mood) String() string { return C.GoString(C.ustore_mood_name(C.ustore_mood_t(mood))) }

// MoodFromName reads a mood back from the spelling [Mood.String] renders.
func MoodFromName(name string) (Mood, error) {
	var mood C.ustore_mood_t
	err := named(C.ustore_mood_from_name(view(name), &mood))
	return Mood(mood), err
}

// CPUClass is the CPU generation a database or an executor confines itself to; the zero value sets
// no ceiling.
type CPUClass int32

const (
	// CPUClassUnknown picks the newest class this machine runs.
	CPUClassUnknown CPUClass = C.ustore_cpu_class_unknown_k
	// CPUClassSerial is the portable floor every CPU runs.
	CPUClassSerial CPUClass = C.ustore_cpu_class_serial_k
	// CPUClassMonoversioned is the one class of a monoversioned build.
	CPUClassMonoversioned CPUClass = C.ustore_cpu_class_monoversioned_k
	// CPUClassIntelHaswell is x86 from Intel Haswell on.
	CPUClassIntelHaswell CPUClass = C.ustore_cpu_class_intel_haswell_k
	// CPUClassIntelSierra is x86 efficiency cores from Intel Sierra Forest on.
	CPUClassIntelSierra CPUClass = C.ustore_cpu_class_intel_sierra_k
	// CPUClassIntelSkylake is x86 server cores from Intel Skylake on.
	CPUClassIntelSkylake CPUClass = C.ustore_cpu_class_intel_skylake_k
	// CPUClassIntelIcelake is x86 server cores from Intel Ice Lake on.
	CPUClassIntelIcelake CPUClass = C.ustore_cpu_class_intel_icelake_k
	// CPUClassAWSG2 is Arm from AWS Graviton 2 on.
	CPUClassAWSG2 CPUClass = C.ustore_cpu_class_aws_g2_k
	// CPUClassAppleM2 is Arm from Apple M2 on.
	CPUClassAppleM2 CPUClass = C.ustore_cpu_class_apple_m2_k
	// CPUClassAWSG5 is Arm server cores from AWS Graviton 5 on.
	CPUClassAWSG5 CPUClass = C.ustore_cpu_class_aws_g5_k
	// CPUClassAlibabaC930 is RISC-V with RVV, Zawrs and Zacas, from Alibaba's C930 on.
	CPUClassAlibabaC930 CPUClass = C.ustore_cpu_class_alibaba_c930_k
	// CPUClassIBMPower10 is Power from POWER10 on.
	CPUClassIBMPower10 CPUClass = C.ustore_cpu_class_ibm_power10_k
)

// String renders the class as the library spells it, such as "intel-haswell".
func (class CPUClass) String() string {
	return C.GoString(C.ustore_cpu_class_name(C.ustore_cpu_class_t(class)))
}

// CPUClassFromName reads a CPU class back from the spelling [CPUClass.String] renders.
func CPUClassFromName(name string) (CPUClass, error) {
	var class C.ustore_cpu_class_t
	err := named(C.ustore_cpu_class_from_name(view(name), &class))
	return CPUClass(class), err
}

// Transport is how the NVMe engine moves bytes between its files and memory.
type Transport int32

const (
	// TransportAutomatic takes io_uring where the kernel offers it, Linux AIO where it offers only
	// that, and blocking calls elsewhere.
	TransportAutomatic Transport = C.ustore_transport_automatic_k
	// TransportBlocking takes positioned reads and writes that block the polling thread.
	TransportBlocking Transport = C.ustore_transport_blocking_k
	// TransportLinuxUring takes io_uring on unbuffered files.
	TransportLinuxUring Transport = C.ustore_transport_linux_uring_k
	// TransportLinuxAIO takes Linux AIO on unbuffered files.
	TransportLinuxAIO Transport = C.ustore_transport_linux_aio_k
	// TransportLinuxSPDK takes SPDK's user-space driver on NVMe devices the process owns.
	TransportLinuxSPDK Transport = C.ustore_transport_linux_spdk_k
)

// String renders the transport as the library spells it.
func (transport Transport) String() string {
	return C.GoString(C.ustore_transport_name(C.ustore_transport_t(transport)))
}

// TransportFromName reads a transport back from the spelling [Transport.String] renders.
func TransportFromName(name string) (Transport, error) {
	var transport C.ustore_transport_t
	err := named(C.ustore_transport_from_name(view(name), &transport))
	return Transport(transport), err
}

// Gds is how a VRAM executor moves bytes through GPUDirect Storage, between the drives and its
// device.
type Gds int32

const (
	// GdsAutomatic transfers directly where the driver is loaded, and through cuFile's compatibility
	// mode elsewhere.
	GdsAutomatic Gds = C.ustore_gds_automatic_k
	// GdsDirect transfers directly, refused with [StatusDevice] where the driver is absent.
	GdsDirect Gds = C.ustore_gds_direct_k
	// GdsCompatibility bounces through host pages cuFile manages.
	GdsCompatibility Gds = C.ustore_gds_compatibility_k
)

// String renders the mode as the library spells it.
func (gds Gds) String() string { return C.GoString(C.ustore_gds_name(C.ustore_gds_t(gds))) }

// GdsFromName reads a GPUDirect Storage mode back from the spelling [Gds.String] renders.
func GdsFromName(name string) (Gds, error) {
	var gds C.ustore_gds_t
	err := named(C.ustore_gds_from_name(view(name), &gds))
	return Gds(gds), err
}

// GPUVendor is a GPU vendor one binary drives, each through a runtime opened on first use.
type GPUVendor int32

const (
	// GPUVendorUnknown names no vendor.
	GPUVendorUnknown GPUVendor = C.ustore_gpu_vendor_unknown_k
	// GPUVendorNVIDIA drives NVIDIA devices through the CUDA driver.
	GPUVendorNVIDIA GPUVendor = C.ustore_gpu_vendor_nvidia_k
	// GPUVendorAMD drives AMD devices through the HIP runtime.
	GPUVendorAMD GPUVendor = C.ustore_gpu_vendor_amd_k
	// GPUVendorApple drives Apple devices through Metal.
	GPUVendorApple GPUVendor = C.ustore_gpu_vendor_apple_k
)

// String renders the vendor as the library spells it.
func (vendor GPUVendor) String() string {
	return C.GoString(C.ustore_gpu_vendor_name(C.ustore_gpu_vendor_t(vendor)))
}

// GPUVendorFromName reads a GPU vendor back from the spelling [GPUVendor.String] renders.
func GPUVendorFromName(name string) (GPUVendor, error) {
	var vendor C.ustore_gpu_vendor_t
	err := named(C.ustore_gpu_vendor_from_name(view(name), &vendor))
	return GPUVendor(vendor), err
}

// GPUClass is a GPU generation UStore tells apart; a class implies its vendor, and the zero value
// sets no ceiling.
type GPUClass int32

const (
	// GPUClassUnknown is no class.
	GPUClassUnknown GPUClass = C.ustore_gpu_class_unknown_k
	// GPUClassNVIDIAAmpere is Ampere-class NVIDIA, and every later GPU that compiles its PTX.
	GPUClassNVIDIAAmpere GPUClass = C.ustore_gpu_class_nvidia_ampere_k
	// GPUClassNVIDIAAda is Ada-class NVIDIA.
	GPUClassNVIDIAAda GPUClass = C.ustore_gpu_class_nvidia_ada_k
	// GPUClassNVIDIAHopper is Hopper-class NVIDIA.
	GPUClassNVIDIAHopper GPUClass = C.ustore_gpu_class_nvidia_hopper_k
	// GPUClassNVIDIABlackwell is Blackwell-class NVIDIA.
	GPUClassNVIDIABlackwell GPUClass = C.ustore_gpu_class_nvidia_blackwell_k
	// GPUClassNVIDIABlackwellRTX is consumer Blackwell NVIDIA.
	GPUClassNVIDIABlackwellRTX GPUClass = C.ustore_gpu_class_nvidia_blackwellrtx_k
	// GPUClassAMDCDNA3 is CDNA3 AMD, gfx942.
	GPUClassAMDCDNA3 GPUClass = C.ustore_gpu_class_amd_cdna3_k
	// GPUClassAMDCDNA4 is CDNA4 AMD, gfx950.
	GPUClassAMDCDNA4 GPUClass = C.ustore_gpu_class_amd_cdna4_k
	// GPUClassAMDCDNA5 is CDNA5 AMD, gfx1250.
	GPUClassAMDCDNA5 GPUClass = C.ustore_gpu_class_amd_cdna5_k
	// GPUClassAppleMetal7 is Apple7-family GPUs, and every Apple GPU without a class of its own.
	GPUClassAppleMetal7 GPUClass = C.ustore_gpu_class_apple_metal7_k
	// GPUClassAppleMetal9 is Apple9-family GPUs.
	GPUClassAppleMetal9 GPUClass = C.ustore_gpu_class_apple_metal9_k
	// GPUClassAppleMetal10 is Apple10-family GPUs.
	GPUClassAppleMetal10 GPUClass = C.ustore_gpu_class_apple_metal10_k
)

// String renders the class as the library spells it, such as "amd-cdna3".
func (class GPUClass) String() string {
	return C.GoString(C.ustore_gpu_class_name(C.ustore_gpu_class_t(class)))
}

// GPUClassFromName reads a GPU class back from the spelling [GPUClass.String] renders.
func GPUClassFromName(name string) (GPUClass, error) {
	var class C.ustore_gpu_class_t
	err := named(C.ustore_gpu_class_from_name(view(name), &class))
	return GPUClass(class), err
}

// Residency is where an arena's outputs live.
type Residency int32

const (
	// ResidencyUnknown is no residency, which only a zeroed description reads as.
	ResidencyUnknown Residency = C.ustore_residency_unknown_k
	// ResidencyRam is host pages, which Go code reads in place.
	ResidencyRam Residency = C.ustore_residency_ram_k
	// ResidencyVram is one device's memory, which no Go code may read.
	ResidencyVram Residency = C.ustore_residency_vram_k
)

// endregion: Enums

// region: Strings

// Tape passes a ragged batch of byte strings as Apache Arrow lays one out: the bytes end to end,
// and one more offset than strings around them, string i spanning offsets[i] up to offsets[i+1].
//
// A Tape views and never owns, and the binding pins both slices until each call it crosses is
// final. The zero Tape is the empty batch, which a verb reads as its input's default.
type Tape struct {
	data    []byte
	offsets []uint32
	wide    []uint64
}

// U32Tape views bytes end to end around len(offsets) - 1 strings, in Arrow's binary layout.
//
// Panics when the offsets overrun the bytes, which names a caller's arithmetic mistake rather than
// a condition the library could report.
func U32Tape(data []byte, offsets []uint32) Tape {
	if len(offsets) == 0 || uint64(offsets[len(offsets)-1]) > uint64(len(data)) {
		panic("ustore: a tape needs one more offset than strings, the last within its bytes")
	}
	return Tape{data: data, offsets: offsets}
}

// U64Tape views bytes end to end around len(offsets) - 1 strings, in Arrow's large binary layout.
//
// Panics when the offsets overrun the bytes.
func U64Tape(data []byte, offsets []uint64) Tape {
	if len(offsets) == 0 || offsets[len(offsets)-1] > uint64(len(data)) {
		panic("ustore: a tape needs one more offset than strings, the last within its bytes")
	}
	return Tape{data: data, wide: offsets}
}

// TapeOf packs strings end to end into a fresh Tape, one entry apiece, with 32-bit offsets while
// the bytes fit in 4 GiB and 64-bit beyond.
func TapeOf[Text ~string | ~[]byte](texts ...Text) Tape {
	total := 0
	for _, entry := range texts {
		total += len(entry)
	}
	data := make([]byte, 0, total)
	if uint64(total) <= 0xFFFFFFFF {
		offsets := make([]uint32, 1, len(texts)+1)
		for _, entry := range texts {
			data = append(data, entry...)
			offsets = append(offsets, uint32(len(data)))
		}
		return Tape{data: data, offsets: offsets}
	}
	offsets := make([]uint64, 1, len(texts)+1)
	for _, entry := range texts {
		data = append(data, entry...)
		offsets = append(offsets, uint64(len(data)))
	}
	return Tape{data: data, wide: offsets}
}

// Len reports how many strings the tape spans.
func (tape Tape) Len() uint {
	if len(tape.wide) != 0 {
		return uint(len(tape.wide)) - 1
	}
	if len(tape.offsets) != 0 {
		return uint(len(tape.offsets)) - 1
	}
	return 0
}

// stringsTape is the tape member of a strings view's data union, which cgo hands over as bytes.
type stringsTape struct {
	data    unsafe.Pointer
	offsets unsafe.Pointer
}

// Strings is a ragged batch a call wrote into its arena as a tape, readable until the arena serves
// another call.
type Strings struct {
	raw   C.ustore_strings_view_t
	count uint
}

// Len reports how many strings the call wrote.
func (strings Strings) Len() uint { return strings.count }

// span reports where string index starts and ends, in bytes.
func (strings Strings) span(index uint) (uint64, uint64) {
	tape := (*stringsTape)(unsafe.Pointer(&strings.raw.data))
	if strings.raw.layout == C.ustore_strings_u64_tape_k {
		offsets := unsafe.Slice((*uint64)(tape.offsets), strings.count+1)
		return offsets[index], offsets[index+1]
	}
	offsets := unsafe.Slice((*uint32)(tape.offsets), strings.count+1)
	return uint64(offsets[index]), uint64(offsets[index+1])
}

// Get views string index in the arena; index must be below [Strings.Len].
func (strings Strings) Get(index uint) []byte {
	begin, end := strings.span(index)
	if begin == end {
		return nil
	}
	tape := (*stringsTape)(unsafe.Pointer(&strings.raw.data))
	return unsafe.Slice((*byte)(unsafe.Add(tape.data, begin)), end-begin)
}

// sized gives a tape the count its call reported, or nothing when the call wrote no tape.
func (strings Strings) sized(count uint) Strings {
	if (*stringsTape)(unsafe.Pointer(&strings.raw.data)).offsets == nil {
		return Strings{}
	}
	strings.count = count
	return strings
}

// Tape passes the strings back as an input, so a tape one call wrote is written again as it is,
// valid while the arena holds it.
func (strings Strings) Tape() Tape {
	if strings.count == 0 {
		return Tape{}
	}
	tape := (*stringsTape)(unsafe.Pointer(&strings.raw.data))
	_, end := strings.span(strings.count - 1)
	data := unsafe.Slice((*byte)(tape.data), end)
	if strings.raw.layout == C.ustore_strings_u64_tape_k {
		return Tape{data: data, wide: unsafe.Slice((*uint64)(tape.offsets), strings.count+1)}
	}
	return Tape{data: data, offsets: unsafe.Slice((*uint32)(tape.offsets), strings.count+1)}
}

// Patterns keeps the values or strings a scan or a walk lists to the ones matching, shared by every
// task of the call; the zero value matches everything.
type Patterns struct {
	// Patterns are the expressions or substrings to match.
	Patterns Tape
	// Kind is what the patterns are matched as.
	Kind PatternKind
	// Sensitivity is whether matching folds case.
	Sensitivity CaseSensitivity
	// Weights holds one weight per substring pattern, which turns on BM25 scores, or nothing.
	Weights []float32
}

// check refuses weights that do not pair one per pattern, which the library would read past.
func (patterns Patterns) check() error {
	if len(patterns.Weights) != 0 && uint(len(patterns.Weights)) != patterns.Patterns.Len() {
		return fail(StatusInvalidArgument, "pass no weights or one per pattern")
	}
	return nil
}

// endregion: Strings

// region: Callbacks

// ProgressCallback is called with the units a long call finished so far and the units it finishes
// if it runs to the end; false stops it, which then fails with [StatusCancelled] and publishes
// nothing. It never runs on two threads at once, and it must not panic.
type ProgressCallback func(finished, total uint64) bool

//export ustoreContinues
func ustoreContinues(finished, total C.ustore_size_t, state unsafe.Pointer) C.ustore_continuation_t {
	if (*cgo.Handle)(state).Value().(ProgressCallback)(uint64(finished), uint64(total)) {
		return C.ustore_continue_k
	}
	return C.ustore_cancel_k
}

// progress passes an optional handler as the callback and state a reporting verb takes.
func (call *call) progress(progress ProgressCallback) (C.ustore_progress_callback_t, unsafe.Pointer) {
	if progress == nil {
		return nil, nil
	}
	return C.ustore_go_continues(), call.callback(1, progress)
}

// callback passes a handler to its C trampoline as an opaque handle in the frame rather than a Go
// pointer, since the closure holds Go pointers the cgo rules forbid handing to C.
func (call *call) callback(slot int, handler any) unsafe.Pointer {
	call.slots.handles[slot] = cgo.NewHandle(handler)
	return unsafe.Pointer(&call.slots.handles[slot])
}

// endregion: Callbacks

// region: Executors

// Executor owns the host threads, I/O rings or device stream calls run on, shared by any number of
// databases.
//
// A nil *Executor is the null executor: it owns nothing, and every call handed it runs on the
// calling goroutine. An executor serves one call at a time, while databases stay freely concurrent;
// a second call finding it held finishes [StatusContended], and nothing queues.
type Executor struct {
	handle C.ustore_executor_t
}

// ExecutorForRam spawns host threads for the RAM, RocksDB and LevelDB engines, interleaved across
// the compute domains local to memoryDomains, every domain the process may allocate on for an
// empty list. Zero threads takes seven eighths of the cores those domains carry, and a
// cpuClassCeiling the CPU lacks narrows the pick rather than refusing it. Every call's staging
// carves from each thread's scratch, capped at scratchLimit, where zero takes 16 GiB.
func ExecutorForRam(memoryDomains []int32, threads uint, cpuClassCeiling CPUClass, scratchLimit uint64) (*Executor, error) {
	var call callWithResult[C.ustore_executor_t]
	arguments := open[C.ustore_executor_for_ram_t](&call)
	*arguments = C.ustore_executor_for_ram_t{
		executor:          call.result,
		outcome:           call.outcome(),
		ram:               call.ram(memoryDomains),
		threads:           C.ustore_size_t(threads),
		cpu_class_ceiling: C.ustore_cpu_class_t(cpuClassCeiling),
		scratch_limit:     C.ustore_size_t(scratchLimit),
	}
	C.ustore_executor_for_ram(arguments)
	return executorOf(block(&call, nil))
}

// NvmeOptions is how the threads of an NVMe executor move bytes between the drives and memory,
// where every zero size or count keeps the engine's default.
type NvmeOptions struct {
	// Transport is how bytes move between the files and memory.
	Transport Transport
	// QueueDepth counts the submissions a channel holds at once.
	QueueDepth uint64
	// RequestBufferBytes sizes each request buffer of a channel.
	RequestBufferBytes uint64
	// RequestBuffersCount counts the request buffers of each channel.
	RequestBuffersCount uint64
	// StreamBufferBytes sizes each streaming buffer of a channel.
	StreamBufferBytes uint64
	// StreamBuffersCount counts the streaming buffers of each channel.
	StreamBuffersCount uint64
	// Paths are the data directories, raw media or SPDK namespaces of the databases the executor
	// will open, read only during the call: the memory domain they share places the threads when no
	// memory domain is named, and their fewest hardware queues give the default thread count.
	Paths []string
}

// ExecutorForNvme spawns threads for the NVMe engine, each submitting and polling its own I/O ring,
// placed with their staging on memoryDomains, the first placing the threads, or on the domain
// nvme.Paths share for an empty list. Zero threads takes the fewest hardware queues of the disks
// under the paths, capped by the cores, and a zero scratchLimit takes 16 GiB per thread.
func ExecutorForNvme(memoryDomains []int32, nvme NvmeOptions, threads uint, cpuClassCeiling CPUClass, scratchLimit uint64) (*Executor, error) {
	var call callWithResult[C.ustore_executor_t]
	arguments := open[C.ustore_executor_for_nvme_t](&call)
	*arguments = C.ustore_executor_for_nvme_t{
		executor: call.result,
		outcome:  call.outcome(),
		nvme: C.ustore_nvme_options_t{
			transport:             C.ustore_transport_t(nvme.Transport),
			queue_depth:           C.ustore_size_t(nvme.QueueDepth),
			request_buffer_bytes:  C.ustore_size_t(nvme.RequestBufferBytes),
			request_buffers_count: C.ustore_size_t(nvme.RequestBuffersCount),
			stream_buffer_bytes:   C.ustore_size_t(nvme.StreamBufferBytes),
			stream_buffers_count:  C.ustore_size_t(nvme.StreamBuffersCount),
			paths:                 call.paths(nvme.Paths),
			paths_count:           C.ustore_size_t(len(nvme.Paths)),
		},
		ram:               call.ram(memoryDomains),
		threads:           C.ustore_size_t(threads),
		cpu_class_ceiling: C.ustore_cpu_class_t(cpuClassCeiling),
		scratch_limit:     C.ustore_size_t(scratchLimit),
	}
	C.ustore_executor_for_nvme(arguments)
	return executorOf(block(&call, nil))
}

// ExecutorForVramConfig carries the optional arguments of [ExecutorForVram].
type ExecutorForVramConfig struct {
	// Stream stays nil: the executor creates and destroys a stream of its own, and a caller's
	// cudaStream_t, hipStream_t or id<MTLCommandQueue> answers [StatusUnsupported], since the
	// executor drives only its own.
	Stream unsafe.Pointer
	// ScratchLimit caps the device scratch a call or a merge takes; zero lets scratch grow.
	ScratchLimit uint64
	// Gds is how reads and compactions move bytes between the drives and the device.
	Gds Gds
	// GPUClassCeiling caps the class dispatch may pick; zero sets no ceiling.
	GPUClassCeiling GPUClass
}

// ExecutorForVram opens a stream of its own on device, its position in the listing of [GPUDevices],
// created and destroyed with the executor and fed by the goroutine that calls the verbs, so that a
// deferred call on it is final once it returns.
func ExecutorForVram(device int32, config *ExecutorForVramConfig) (*Executor, error) {
	options := configOf(config)
	var call callWithResult[C.ustore_executor_t]
	arguments := open[C.ustore_executor_for_vram_t](&call)
	*arguments = C.ustore_executor_for_vram_t{
		executor:          call.result,
		outcome:           call.outcome(),
		device:            C.int32_t(device),
		stream:            options.Stream,
		scratch_limit:     C.ustore_size_t(options.ScratchLimit),
		gds:               C.ustore_gds_t(options.Gds),
		gpu_class_ceiling: C.ustore_gpu_class_t(options.GPUClassCeiling),
	}
	C.ustore_executor_for_vram(arguments)
	return executorOf(block(&call, nil))
}

// executorOf wraps the handle an executor factory wrote.
func executorOf(handle C.ustore_executor_t, err error) (*Executor, error) {
	if err != nil {
		return nil, err
	}
	return &Executor{handle: handle}, nil
}

// Drain waits until no call holds the executor, then adopts mood until its next call; it only waits
// and never runs a call itself. A failure is the executor's own, never one of a drained call.
func (executor *Executor) Drain(mood Mood) error {
	handle, err := executor.lower()
	if err != nil {
		return err
	}
	var call callWithResult[struct{}]
	arguments := open[C.ustore_executor_drain_t](&call)
	*arguments = C.ustore_executor_drain_t{
		executor: handle,
		outcome:  call.outcome(),
		mood:     C.ustore_mood_t(mood),
	}
	C.ustore_executor_drain(arguments)
	_, err = block(&call, nil)
	return err
}

// GPUDevice is one GPU as UStore numbers it: its position in [GPUDevices] is the ordinal
// [ExecutorForVram] names it by.
type GPUDevice struct {
	// Vendor is the vendor whose runtime drives the device.
	Vendor GPUVendor
	// VendorOrdinal is the device's ordinal in its vendor's runtime.
	VendorOrdinal uint32
	// GPUClass is the newest class this build compiled code for that runs on the device.
	GPUClass GPUClass
	// ComputeUnits counts streaming multiprocessors, compute units or GPU cores.
	ComputeUnits uint32
	// MemoryBytes is the device memory, or the working set Metal recommends for unified memory.
	MemoryBytes uint64
	// Name is the vendor's own name for the device.
	Name string
}

// GPUDevices lists every GPU this process can drive, NVIDIA devices first, then AMD, then Apple,
// and nothing on a machine without GPUs.
func GPUDevices() ([]GPUDevice, error) {
	var counted callWithResult[C.ustore_size_t]
	counting := open[C.ustore_gpu_devices_t](&counted)
	*counting = C.ustore_gpu_devices_t{
		outcome:       counted.outcome(),
		devices_count: counted.result,
	}
	C.ustore_gpu_devices(counting)
	count, err := block(&counted, nil)
	if err != nil || count == 0 {
		return nil, err
	}
	raw := make([]C.ustore_gpu_device_t, count)
	var listed callWithResult[C.ustore_size_t]
	listing := open[C.ustore_gpu_devices_t](&listed)
	*listing = C.ustore_gpu_devices_t{
		outcome:          listed.outcome(),
		devices:          (*C.ustore_gpu_device_t)(listed.pinSlice(unsafe.Pointer(unsafe.SliceData(raw)), len(raw))),
		devices_capacity: count,
		devices_count:    listed.result,
	}
	C.ustore_gpu_devices(listing)
	count, err = block(&listed, nil)
	if err != nil {
		return nil, err
	}
	devices := make([]GPUDevice, min(uint64(count), uint64(len(raw))))
	for index := range devices {
		device := &raw[index]
		devices[index] = GPUDevice{
			Vendor:        GPUVendor(device.vendor),
			VendorOrdinal: uint32(device.vendor_ordinal),
			GPUClass:      GPUClass(device.gpu_class),
			ComputeUnits:  uint32(device.compute_units),
			MemoryBytes:   uint64(device.memory_bytes),
			Name:          C.GoString(&device.name[0]),
		}
	}
	return devices, nil
}

// endregion: Executors

// region: Arenas

// Arena is memory the library writes every output whose length scales with a call into, reused so a
// steady loop allocates once.
//
// Every call given an arena rewinds and refills it, so the views of its previous call and every
// slice they handed out end there, as they do once it is closed; to keep one call's views past the
// next, give that call another arena. An arena serves one call at a time, and a second overlapping
// it fails with [StatusContended]. Deferred verbs view it as blocking ones do, since Close waits
// for every deferred call naming it.
type Arena struct {
	handle C.ustore_arena_t
}

// NewArena makes an empty arena where executor computes, host pages or the memory of its GPU,
// decided once; a nil executor makes host pages for calls on the calling goroutine. Nothing is
// allocated until a call writes into it. An arena must not outlive the executor that made it, so
// [Arena.Close] comes before [Executor.Close]; one made with a nil executor lives until it is
// closed.
func NewArena(executor *Executor) (*Arena, error) {
	handle, err := executor.lower()
	if err != nil {
		return nil, err
	}
	var call callWithResult[C.ustore_arena_t]
	arguments := open[C.ustore_arena_init_t](&call)
	*arguments = C.ustore_arena_init_t{
		arena:    call.result,
		executor: handle,
		outcome:  call.outcome(),
	}
	C.ustore_arena_init(arguments)
	arena, err := block(&call, nil)
	if err != nil {
		return nil, err
	}
	return &Arena{handle: arena}, nil
}

// ArenaDescription is where an arena's outputs live, which decides whether Go code may read them,
// laid out as ustore_arena_description_t.
type ArenaDescription struct {
	// Residency is [ResidencyRam] for host pages and [ResidencyVram] for one device's memory.
	Residency Residency
	// Device is the ordinal of the GPU holding the outputs, as [GPUDevices] lists it, or -1 for host
	// pages.
	Device int32
}

// The library writes the description in place, so the two layouts must agree.
var _ = [1]struct{}{}[unsafe.Sizeof(ArenaDescription{})-unsafe.Sizeof(C.ustore_arena_description_t{})]

// Describe reports where the arena's outputs live, as the executor that made it decided.
func (arena *Arena) Describe() (ArenaDescription, error) {
	handle, err := arena.lower()
	if err != nil {
		return ArenaDescription{}, err
	}
	var call callWithResult[ArenaDescription]
	arguments := open[C.ustore_arena_describe_t](&call)
	*arguments = C.ustore_arena_describe_t{
		arena:       handle,
		outcome:     call.outcome(),
		description: (*C.ustore_arena_description_t)(unsafe.Pointer(call.result)),
	}
	C.ustore_arena_describe(arguments)
	return block(&call, nil)
}

// endregion: Arenas

// region: Calls

// slots is what the library reads and writes for every verb: the outcome, and the handles its
// callbacks reach Go through.
type slots struct {
	outcome C.ustore_outcome_t
	handles [2]cgo.Handle
}

// frame is one call's slots, argument structure and result in a single pinned allocation, whose
// only Go pointers are those the arguments hold, every one of them pinned until the call settles.
type frame[Arguments, Result any] struct {
	slots
	arguments Arguments
	result    Result
}

// call is one verb crossing into the library: the pins that keep every Go pointer its argument
// structure carries in place until the call is final, and the slots it reports through. cgo lets C
// keep a Go pointer only while it is pinned, and pinning is also what keeps every pointee on the
// heap, which the noescape promises rely on.
type call struct {
	pinner runtime.Pinner
	slots  *slots
	// arguments is the frame's argument structure, cleared once the call is final, so a settled
	// frame holds no Go pointer and its outcome may still cross to C unpinned.
	arguments []byte
}

// callWithResult is a call and the slot its result lands in, which the library writes in place.
type callWithResult[Result any] struct {
	call
	result *Result
}

// open pins a fresh frame and hands back its argument structure, which a verb fills and passes to
// C; verbs refuse before opening, so a refusal pins and allocates nothing. The arguments live in
// the frame because cgo moves to the heap whatever a C function that may call back into Go reaches.
func open[Arguments, Result any](opened *callWithResult[Result]) *Arguments {
	frame := new(frame[Arguments, Result])
	opened.pinner.Pin(frame)
	opened.slots, opened.result = &frame.slots, &frame.result
	opened.arguments = unsafe.Slice((*byte)(unsafe.Pointer(&frame.arguments)), unsafe.Sizeof(frame.arguments))
	return &frame.arguments
}

// outcome is where the library reports the call's status.
func (call *call) outcome() *C.ustore_outcome_t { return &call.slots.outcome }

// status reads the outcome word without waiting.
func (call *call) status() Status {
	return Status(C.ustore_outcome_status(call.outcome()))
}

// settle waits until the call is final, releases everything it call, and reports its failure, the
// same every time it is asked.
func (call *call) settle() error {
	if call.status() == StatusDeferred {
		C.ustore_outcome_wait(call.outcome())
	}
	for slot, handle := range call.slots.handles {
		if handle != 0 {
			handle.Delete()
			call.slots.handles[slot] = 0
		}
	}
	clear(call.arguments)
	call.pinner.Unpin()
	return check(call.slots.outcome)
}

// block settles the call a verb opened and hands back its result, or the verb's refusal.
func block[Result any](call *callWithResult[Result], refusal error) (Result, error) {
	var none Result
	if refusal != nil {
		return none, refusal
	}
	if err := call.settle(); err != nil {
		return none, err
	}
	return *call.result, nil
}

// configOf reads a nil config as the zero one, by value, so generic verbs allocate nothing for it.
func configOf[Config any](config *Config) Config {
	var options Config
	if config != nil {
		options = *config
	}
	return options
}

// pin keeps a Go object in place until the call is final and hands back its address. It takes no
// type parameter, since a generic call inlined into a generic verb reaches its callee through a
// dictionary, and escape analysis then moves the verb's call to the heap.
func (call *call) pin(pointer unsafe.Pointer) unsafe.Pointer {
	call.pinner.Pin(pointer)
	return pointer
}

// pinSlice pins the backing array of a slice of length elements at data until the call is final,
// and passes an empty slice as nil.
func (call *call) pinSlice(data unsafe.Pointer, length int) unsafe.Pointer {
	if length == 0 {
		return nil
	}
	return call.pin(data)
}

// view passes a string to a call that only reads it before returning.
func view(text string) C.ustore_bytes_view_t {
	view := C.ustore_bytes_view_t{data: (*C.uint8_t)(unsafe.Pointer(unsafe.StringData(text)))}
	// cgo names the length's typedef apart from size_t, though the two are as wide.
	*(*C.size_t)(unsafe.Pointer(&view.length)) = C.size_t(len(text))
	return view
}

// Deferred is a submitted call, which holds its outcome and inputs until it is final, owned by the
// one goroutine that polls or waits on it. Status polls without blocking, Wait blocks until the
// call is final and hands back its result, and Close waits the same way and drops it. A Deferred
// dropped without Wait or Close is released by the collector once the call is final, never
// blocking on it: until then every later collection checks it again.
type Deferred[Result any] struct {
	call    callWithResult[Result]
	cleanup runtime.Cleanup
}

// submitted hands the call a verb opened to its caller, who waits on it or closes it, or drops it
// to the collector; a verb's refusal comes back instead, with nothing submitted.
func submitted[Result any](deferred *Deferred[Result], refusal error) (*Deferred[Result], error) {
	if refusal != nil {
		return nil, refusal
	}
	deferred.cleanup = runtime.AddCleanup(deferred, release, deferred.call.call)
	return deferred, nil
}

// release settles a dropped call once it is final, and otherwise arms itself again for the next
// collection, since the collector's goroutine must not block.
func release(call call) {
	if call.status() == StatusDeferred {
		runtime.AddCleanup(&call, release, call)
		return
	}
	_ = call.settle()
}

// Status reports StatusDeferred until the call is final, then its final status, without blocking.
func (deferred *Deferred[Result]) Status() Status { return deferred.call.status() }

// Wait blocks until the call is final, releases its pins, then hands back its result, the same
// every time it is asked.
func (deferred *Deferred[Result]) Wait() (Result, error) {
	deferred.cleanup.Stop()
	return block(&deferred.call, nil)
}

// Close blocks until the call is final, releases its pins, and reports the call's failure. It is
// idempotent and safe after Wait.
func (deferred *Deferred[Result]) Close() error {
	deferred.cleanup.Stop()
	return deferred.call.settle()
}

// endregion: Calls

// region: Receivers

// receiver is a handle's field, so a Close is seen, and the executor its verbs run on.
type receiver[Handle comparable] struct {
	handle   *Handle
	executor *Executor
}

// lower reports the handles a verb hands across, refusing a closed handle before C dereferences it.
func (receiver receiver[Handle]) lower() (Handle, C.ustore_executor_t, error) {
	var none Handle
	if receiver.handle == nil || *receiver.handle == none {
		return none, nil, fail(StatusUninitialized, "the handle is closed or was never initialized")
	}
	executor, err := receiver.executor.lower()
	return *receiver.handle, executor, err
}

// lowerInto is lower for a verb writing into arena, refusing a closed one too.
func (receiver receiver[Handle]) lowerInto(arena *Arena) (Handle, C.ustore_executor_t, C.ustore_arena_t, error) {
	handle, executor, err := receiver.lower()
	if err != nil {
		return handle, executor, nil, err
	}
	outputs, err := arena.lower()
	return handle, executor, outputs, err
}

// lower reports the handle a verb hands across: nil for the null executor, and a refusal for a
// closed one rather than a silent fall back to the calling goroutine.
func (executor *Executor) lower() (C.ustore_executor_t, error) {
	if executor == nil {
		return nil, nil
	}
	if executor.handle == nil {
		return nil, fail(StatusUninitialized, "the executor is closed or was never spawned")
	}
	return executor.handle, nil
}

// Close waits out the call holding the executor, then releases its threads, rings or streams; every
// arena it made must be closed first. It is idempotent; a closed executor must not be used again.
func (executor *Executor) Close() error {
	if executor == nil || executor.handle == nil {
		return nil
	}
	C.ustore_executor_free(executor.handle)
	executor.handle = nil
	return nil
}

// lower reports the handle a verb hands across: nil for a nil arena, which the call refuses, and a
// refusal for a closed one.
func (arena *Arena) lower() (C.ustore_arena_t, error) {
	if arena == nil {
		return nil, nil
	}
	if arena.handle == nil {
		return nil, fail(StatusUninitialized, "the arena is closed or was never made")
	}
	return arena.handle, nil
}

// Close releases the memory, waiting for every deferred call naming the arena, and ends every view
// over it; it comes before the Close of the executor that made the arena. It is idempotent, and a
// closed arena must not be used again.
func (arena *Arena) Close() error {
	if arena == nil || arena.handle == nil {
		return nil
	}
	C.ustore_arena_free(arena.handle)
	arena.handle = nil
	return nil
}

// endregion: Receivers

// region: Helpers

// inputs is what a verb passes the library by address but holds by value: the collection and the
// role every task shares, and the end key of a walk.
type inputs struct {
	collection C.ustore_collection_id_t
	key        C.ustore_key_t
	role       C.ustore_vertex_role_t
}

// staged is a verb's argument structure beside the fixed-width inputs it points into, in one frame.
type staged[Arguments any] struct {
	arguments Arguments
	inputs    inputs
}

// stage opens a call whose argument structure passes inputs by address from its own frame.
func stage[Arguments, Result any](call *callWithResult[Result]) (*Arguments, *inputs) {
	frame := open[staged[Arguments]](call)
	return &frame.arguments, &frame.inputs
}

// ram pins the memory domains of an executor, copied by the call.
func (call *call) ram(memoryDomains []int32) C.ustore_ram_options_t {
	return C.ustore_ram_options_t{
		memory_domains:       (*C.int32_t)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(memoryDomains)), len(memoryDomains))),
		memory_domains_count: C.ustore_size_t(len(memoryDomains)),
	}
}

// text pins a Go string as the pointer-and-length pair a path or a name takes. The string is never
// terminated, by contract, so nothing is copied and nothing is freed.
func (call *call) text(text string) C.ustore_bytes_view_t {
	if len(text) == 0 {
		return C.ustore_bytes_view_t{}
	}
	return C.ustore_bytes_view_t{
		data:   (*C.uint8_t)(call.pin(unsafe.Pointer(unsafe.StringData(text)))),
		length: C.ustore_size_t(len(text)),
	}
}

// paths pins the views of the media paths an NVMe executor reads during its creation.
func (call *call) paths(paths []string) *C.ustore_bytes_view_t {
	if len(paths) == 0 {
		return nil
	}
	views := make([]C.ustore_bytes_view_t, len(paths))
	for index, path := range paths {
		views[index] = call.text(path)
	}
	return (*C.ustore_bytes_view_t)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(views)), len(views)))
}

// strings pins a tape as the view a verb reads, zeroed for the empty batch.
func (call *call) strings(tape Tape) C.ustore_strings_view_t {
	var view C.ustore_strings_view_t
	lowered := (*stringsTape)(unsafe.Pointer(&view.data))
	switch {
	case len(tape.wide) != 0:
		view.layout = C.ustore_strings_u64_tape_k
		lowered.data, lowered.offsets = call.pinSlice(unsafe.Pointer(unsafe.SliceData(tape.data)), len(tape.data)), call.pinSlice(unsafe.Pointer(unsafe.SliceData(tape.wide)), len(tape.wide))
	case len(tape.offsets) != 0:
		view.layout = C.ustore_strings_u32_tape_k
		lowered.data, lowered.offsets = call.pinSlice(unsafe.Pointer(unsafe.SliceData(tape.data)), len(tape.data)), call.pinSlice(unsafe.Pointer(unsafe.SliceData(tape.offsets)), len(tape.offsets))
	}
	return view
}

// patterns pins the patterns a scan or a walk matches, zeroed to match everything.
func (call *call) patterns(patterns Patterns) C.ustore_patterns_t {
	return C.ustore_patterns_t{
		patterns:    call.strings(patterns.Patterns),
		count:       C.ustore_size_t(patterns.Patterns.Len()),
		kind:        C.ustore_pattern_kind_t(patterns.Kind),
		sensitivity: C.ustore_case_sensitivity_t(patterns.Sensitivity),
		weights:     (*C.float)(call.pinSlice(unsafe.Pointer(unsafe.SliceData(patterns.Weights)), len(patterns.Weights))),
	}
}

// endregion: Helpers

// region: Version

// Version reports the major, minor and patch version of the library the program runs against.
func Version() (major, minor, patch int) {
	var parts [3]C.int
	C.ustore_version(&parts[0], &parts[1], &parts[2])
	return int(parts[0]), int(parts[1]), int(parts[2])
}

// VersionName reports the version of the library the program runs against, pre-release suffix
// included, such as "1.0.0-rc.1".
func VersionName() string { return C.GoString(C.ustore_version_name()) }

// endregion: Version

// region: Children

// childReceiver is a cursor's or a stream's handle field and the parent handle it was opened on,
// which every pull names, so a Close of either is seen, and the executor a pull runs on, nil for
// the calling goroutine; the child keeps none.
type childReceiver[Child, Parent comparable] struct {
	receiver[Child]
	parent *Parent
}

// lowerInto reports the handles a pull hands across, refusing a closed child, parent, executor or
// arena before C dereferences them.
func (on childReceiver[Child, Parent]) lowerInto(arena *Arena) (Child, Parent, C.ustore_executor_t, C.ustore_arena_t, error) {
	var none Parent
	child, executor, outputs, err := on.receiver.lowerInto(arena)
	if err != nil {
		return child, none, executor, outputs, err
	}
	if *on.parent == none {
		return child, none, executor, outputs, fail(StatusUninitialized, "the handle it was opened on is closed")
	}
	return child, *on.parent, executor, outputs, nil
}

// endregion: Children
