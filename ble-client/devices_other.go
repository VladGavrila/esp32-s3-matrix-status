//go:build !darwin

package main

import "errors"

func captureState() (camera, mic bool, err error) {
	return false, false, errors.New("watch is only supported on macOS")
}
