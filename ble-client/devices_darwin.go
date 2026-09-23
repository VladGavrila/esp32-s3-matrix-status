package main

/*
#cgo LDFLAGS: -framework CoreAudio -framework CoreMediaIO -framework CoreFoundation
#include <stdlib.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMediaIO/CMIOHardware.h>

// Returns 1 if any process is capturing audio input, 0 if not, -1 on error.
// Uses the per-process kAudioProcessPropertyIsRunningInput (macOS 14.2+),
// which is what drives the orange mic indicator. Output-only activity on a
// device that also has inputs (e.g. music playing through a USB headset)
// doesn't count.
static int processMicInUse(void) {
	if (__builtin_available(macOS 14.2, *)) {
		AudioObjectPropertyAddress listAddr = {
			kAudioHardwarePropertyProcessObjectList,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMain,
		};
		UInt32 size = 0;
		if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr, 0, NULL, &size) != noErr) return -1;
		if (size == 0) return 0;
		AudioObjectID *ids = malloc(size);
		if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, NULL, &size, ids) != noErr) {
			free(ids);
			return -1;
		}
		int inUse = 0;
		AudioObjectPropertyAddress runAddr = {
			kAudioProcessPropertyIsRunningInput,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMain,
		};
		for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
			UInt32 running = 0, runSize = sizeof(running);
			if (AudioObjectGetPropertyData(ids[i], &runAddr, 0, NULL, &runSize, &running) == noErr && running) {
				inUse = 1;
				break;
			}
		}
		free(ids);
		return inUse;
	}
	return -1;
}

// Fallback for macOS < 14.2: any device with input streams that's running
// somewhere. Can false-positive on devices that also have outputs.
static int deviceMicInUse(void) {
	AudioObjectPropertyAddress listAddr = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain,
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr, 0, NULL, &size) != noErr) return -1;
	if (size == 0) return 0;
	AudioObjectID *ids = malloc(size);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, NULL, &size, ids) != noErr) {
		free(ids);
		return -1;
	}
	int inUse = 0;
	AudioObjectPropertyAddress streamsAddr = {
		kAudioDevicePropertyStreams,
		kAudioObjectPropertyScopeInput,
		kAudioObjectPropertyElementMain,
	};
	AudioObjectPropertyAddress runAddr = {
		kAudioDevicePropertyDeviceIsRunningSomewhere,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain,
	};
	for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
		UInt32 streamsSize = 0;
		if (AudioObjectGetPropertyDataSize(ids[i], &streamsAddr, 0, NULL, &streamsSize) != noErr || streamsSize == 0) continue;
		UInt32 running = 0, runSize = sizeof(running);
		if (AudioObjectGetPropertyData(ids[i], &runAddr, 0, NULL, &runSize, &running) == noErr && running) {
			inUse = 1;
			break;
		}
	}
	free(ids);
	return inUse;
}

static int micInUse(void) {
	int r = processMicInUse();
	return r >= 0 ? r : deviceMicInUse();
}

// Returns 1 if any camera (built-in, USB, Continuity, virtual) is in use by
// any process, 0 if not, -1 on error. Same signal as the green indicator.
static int cameraInUse(void) {
	CMIOObjectPropertyAddress listAddr = {
		kCMIOHardwarePropertyDevices,
		kCMIOObjectPropertyScopeGlobal,
		kCMIOObjectPropertyElementMain,
	};
	UInt32 size = 0;
	if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &listAddr, 0, NULL, &size) != kCMIOHardwareNoError) return -1;
	if (size == 0) return 0;
	CMIOObjectID *ids = malloc(size);
	UInt32 used = 0;
	if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &listAddr, 0, NULL, size, &used, ids) != kCMIOHardwareNoError) {
		free(ids);
		return -1;
	}
	int inUse = 0;
	CMIOObjectPropertyAddress runAddr = {
		kCMIODevicePropertyDeviceIsRunningSomewhere,
		kCMIOObjectPropertyScopeWildcard,
		kCMIOObjectPropertyElementWildcard,
	};
	for (UInt32 i = 0; i < used / sizeof(CMIOObjectID); i++) {
		UInt32 running = 0, runUsed = 0;
		if (CMIOObjectGetPropertyData(ids[i], &runAddr, 0, NULL, sizeof(running), &runUsed, &running) == kCMIOHardwareNoError && running) {
			inUse = 1;
			break;
		}
	}
	free(ids);
	return inUse;
}
*/
import "C"

import "errors"

// captureState reports whether any camera and any microphone are currently
// in use by any process on this Mac.
func captureState() (camera, mic bool, err error) {
	c := C.cameraInUse()
	if c < 0 {
		return false, false, errors.New("could not query camera state (CoreMediaIO)")
	}
	m := C.micInUse()
	if m < 0 {
		return false, false, errors.New("could not query microphone state (CoreAudio)")
	}
	return c == 1, m == 1, nil
}
