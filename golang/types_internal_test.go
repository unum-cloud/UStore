// The guard every verb runs before it views an arena's outputs as Go slices.
//
// File: golang/types_internal_test.go
// Author: Ash Vardanian
// Date: November 26, 2026

package ustore

import (
	"errors"
	"testing"
)

// Host reads follow HostAccess, not the residency: device memory refuses, shared memory reads.
// No stub arena reports device memory, so the descriptions are built by hand.
func TestHostReadsOfDeviceMemoryRefuse(t *testing.T) {
	device := ArenaDescription{Residency: ResidencyVram, Device: 0, Vendor: GPUVendorNVIDIA, VendorOrdinal: 0, HostAccess: HostAccessRefused}
	if err := device.host(); !errors.Is(err, StatusDevice) {
		t.Fatalf("device memory answered %v", err)
	}
	shared := device
	shared.Vendor, shared.HostAccess = GPUVendorApple, HostAccessReadable
	if err := shared.host(); err != nil {
		t.Fatalf("Apple's shared memory answered %v", err)
	}
}
