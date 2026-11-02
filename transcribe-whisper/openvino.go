package main

// whisper.cpp can run the encoder through OpenVINO, which the Go binding has no call for.

/*
#include <stdlib.h>
#include <whisper.h>
*/
import "C"

import (
	"fmt"
	"strings"
	"sync"
	"unsafe"

	whisper "github.com/ggerganov/whisper.cpp/bindings/go"
)

// openvinoDevice is the OpenVINO device for the encoder, such as CPU, GPU or NPU,
// or empty when whisper.cpp was built without OpenVINO.
func openvinoDevice() string {
	if !hasOpenVINO() {
		return ""
	}
	return getenv("OPENVINO_DEVICE", "CPU")
}

var hasOpenVINO = sync.OnceValue(func() bool {
	return strings.Contains(whisper.Whisper_print_system_info(), "OPENVINO = 1")
})

// initOpenVINO moves m's encoder to OpenVINO on device.
// whisper.cpp finds the converted encoder next to the ggml model, and caches its compiled form there too.
func initOpenVINO(m *whisper.Context, device string) error {
	d := C.CString(device)
	defer C.free(unsafe.Pointer(d))
	if C.whisper_ctx_init_openvino_encoder((*C.struct_whisper_context)(unsafe.Pointer(m)), nil, d, nil) != 0 {
		// whisper.cpp prints OpenVINO's reason to stdout
		return fmt.Errorf("OpenVINO encoder failed to load on %s: is the device's driver installed, and the encoder converted (`mise run model`)?", device)
	}
	return nil
}
