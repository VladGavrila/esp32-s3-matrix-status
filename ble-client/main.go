// ble-client controls the ESP32-S3-Matrix LED server over BLE (default) or
// WiFi/HTTP (with -wifi). Over BLE there's no OS-level Bluetooth pairing:
// it scans for the device by name, connects, sends a command, prints the
// resulting status, and disconnects.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"
)

const (
	deviceName     = "VGS3A"
	defaultHost    = "VGS3A.local"
	defaultTimeout = 3 * time.Second
	apiKeyEnv      = "MATRIX_API_KEY"
)

var (
	adapter = bluetooth.DefaultAdapter

	serviceUUID     = mustParseUUID("8f2b1000-2c4a-4bfa-93be-0a8b3e2a9a01")
	commandCharUUID = mustParseUUID("8f2b1001-2c4a-4bfa-93be-0a8b3e2a9a01")
	statusCharUUID  = mustParseUUID("8f2b1002-2c4a-4bfa-93be-0a8b3e2a9a01")
)

func mustParseUUID(s string) bluetooth.UUID {
	uuid, err := bluetooth.ParseUUID(s)
	if err != nil {
		panic(err)
	}
	return uuid
}

func main() {
	if len(os.Args) < 2 {
		printUsage()
		os.Exit(1)
	}

	switch os.Args[1] {
	case "color":
		runColor(os.Args[2:])
	case "brightness":
		runBrightness(os.Args[2:])
	case "mode":
		runMode(os.Args[2:])
	case "watch":
		runWatch(os.Args[2:])
	case "-h", "--help", "help":
		printUsage()
	default:
		printUsage()
		os.Exit(1)
	}
}

func printUsage() {
	fmt.Fprintln(os.Stderr, `ble-client: control the ESP32-S3-Matrix over BLE (default) or WiFi

Usage:
  ble-client color <r> <g> <b> [-brightness N] [-wifi] [-host HOST] [-timeout D] [-api-key KEY]
  ble-client brightness <level> [-wifi] [-host HOST] [-timeout D] [-api-key KEY]
  ble-client mode <busy|dnd|free|dndmic|test|off> [-wifi] [-host HOST] [-timeout D] [-api-key KEY]
  ble-client watch [-interval D] [-settle D] [-dry-run] [-wifi] [-host HOST] [-timeout D] [-api-key KEY]

By default, commands are sent over BLE. Pass -wifi to send them over the
HTTP API instead (to the device at -host, default "VGS3A.local").

-timeout sets how long to wait for the device (BLE scan, or the HTTP
request) before giving up. Default 3s; accepts durations like "500ms" or
"10s".

watch (macOS only) runs until interrupted, following this Mac's camera and
microphone: camera in use -> dnd, mic only -> dndmic, neither -> busy.

API key defaults to the MATRIX_API_KEY environment variable.`)
}

func fatal(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func must(err error) {
	if err != nil {
		fatal(err.Error())
	}
}

func mustAtoi(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		fatal(fmt.Sprintf("invalid integer %q", s))
	}
	return n
}

func newFlagSet(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ExitOnError)
	fs.Usage = printUsage
	return fs
}

// reorderArgs moves every recognized flag (and, for flags that take a
// value, the token after it) ahead of the positional arguments. Go's flag
// package stops parsing at the first non-flag token, so without this,
// "mode busy -wifi" would treat "-wifi" as a second positional argument
// instead of a flag. fs must already have its flags defined.
func reorderArgs(fs *flag.FlagSet, args []string) []string {
	var flags, positional []string
	for i := 0; i < len(args); i++ {
		arg := args[i]
		if len(arg) < 2 || arg[0] != '-' {
			positional = append(positional, arg)
			continue
		}
		flags = append(flags, arg)

		name := strings.TrimLeft(arg, "-")
		if eq := strings.IndexByte(name, '='); eq >= 0 {
			continue // "-flag=value" is self-contained
		}
		if f := fs.Lookup(name); f != nil {
			isBool := false
			if bf, ok := f.Value.(interface{ IsBoolFlag() bool }); ok {
				isBool = bf.IsBoolFlag()
			}
			if !isBool && i+1 < len(args) {
				i++
				flags = append(flags, args[i])
			}
		}
	}
	return append(flags, positional...)
}

// transportFlags holds the flags shared by every subcommand for choosing
// and configuring the transport (BLE by default, WiFi/HTTP with -wifi).
type transportFlags struct {
	apiKey  *string
	wifi    *bool
	host    *string
	timeout *time.Duration
}

func addTransportFlags(fs *flag.FlagSet) *transportFlags {
	return &transportFlags{
		apiKey:  fs.String("api-key", os.Getenv(apiKeyEnv), "API key (defaults to "+apiKeyEnv+" env var)"),
		wifi:    fs.Bool("wifi", false, "send the command over WiFi/HTTP instead of BLE"),
		host:    fs.String("host", defaultHost, "device hostname or IP (WiFi mode only)"),
		timeout: fs.Duration("timeout", defaultTimeout, "how long to wait for the device (e.g. 3s, 500ms)"),
	}
}

func requireAPIKey(key string) {
	if key == "" {
		fatal("no API key provided; pass -api-key or set the " + apiKeyEnv + " environment variable")
	}
}

func runColor(args []string) {
	fs := newFlagSet("color")
	t := addTransportFlags(fs)
	brightness := fs.Int("brightness", -1, "optional brightness override (0-255)")
	fs.Parse(reorderArgs(fs, args))

	rest := fs.Args()
	if len(rest) != 3 {
		fatal("usage: ble-client color <r> <g> <b> [-brightness N]")
	}
	r, g, b := mustAtoi(rest[0]), mustAtoi(rest[1]), mustAtoi(rest[2])
	requireAPIKey(*t.apiKey)

	cmdArgs := fmt.Sprintf("%d,%d,%d", r, g, b)
	if *brightness >= 0 {
		cmdArgs += fmt.Sprintf(",%d", *brightness)
	}

	must(sendCommand(*t.apiKey, "color", cmdArgs, *t.wifi, *t.host, *t.timeout))
}

func runBrightness(args []string) {
	fs := newFlagSet("brightness")
	t := addTransportFlags(fs)
	fs.Parse(reorderArgs(fs, args))

	rest := fs.Args()
	if len(rest) != 1 {
		fatal("usage: ble-client brightness <level>")
	}
	level := mustAtoi(rest[0])
	requireAPIKey(*t.apiKey)

	must(sendCommand(*t.apiKey, "brightness", strconv.Itoa(level), *t.wifi, *t.host, *t.timeout))
}

func runMode(args []string) {
	fs := newFlagSet("mode")
	t := addTransportFlags(fs)
	fs.Parse(reorderArgs(fs, args))

	rest := fs.Args()
	if len(rest) != 1 {
		fatal("usage: ble-client mode <busy|dnd|free|dndmic|test|off>")
	}
	name := rest[0]
	switch name {
	case "busy", "dnd", "free", "dndmic", "test", "off":
	default:
		fatal(fmt.Sprintf("unknown mode %q, expected busy, dnd, free, dndmic, test, or off", name))
	}
	requireAPIKey(*t.apiKey)

	must(sendCommand(*t.apiKey, "mode", name, *t.wifi, *t.host, *t.timeout))
}

const watchRetryDelay = 5 * time.Second

// watchMode picks the mode for the current capture state. The camera wins
// over the mic, and idle falls back to busy rather than free.
func watchMode(camera, mic bool) string {
	switch {
	case camera:
		return "dnd"
	case mic:
		return "dndmic"
	default:
		return "busy"
	}
}

func runWatch(args []string) {
	fs := newFlagSet("watch")
	t := addTransportFlags(fs)
	interval := fs.Duration("interval", time.Second, "how often to check camera/mic state")
	settle := fs.Duration("settle", 2*time.Second, "how long a new state must hold before it's sent")
	dryRun := fs.Bool("dry-run", false, "log mode changes without sending them")
	fs.Parse(reorderArgs(fs, args))

	if len(fs.Args()) != 0 {
		fatal("usage: ble-client watch [-interval D] [-settle D] [-dry-run]")
	}
	if !*dryRun {
		requireAPIKey(*t.apiKey)
	}

	// sent is the last mode the device accepted; candidate is what the
	// camera/mic currently call for, and must hold for -settle before it's
	// sent (calls often flick the mic on a moment before the camera).
	var sent, candidate string
	var candidateSince, nextAttempt time.Time

	ticker := time.NewTicker(*interval)
	defer ticker.Stop()
	for ; ; <-ticker.C {
		camera, mic, err := captureState()
		if err != nil {
			log.Print(err)
			continue
		}

		now := time.Now()
		if mode := watchMode(camera, mic); mode != candidate {
			candidate, candidateSince = mode, now
		}
		if candidate == sent || now.Sub(candidateSince) < *settle || now.Before(nextAttempt) {
			continue
		}

		log.Printf("camera=%v mic=%v -> mode %s", camera, mic, candidate)
		if *dryRun {
			sent = candidate
			continue
		}
		if err := sendCommand(*t.apiKey, "mode", candidate, *t.wifi, *t.host, *t.timeout); err != nil {
			log.Printf("sending mode %s failed: %v (retrying in %s)", candidate, err, watchRetryDelay)
			nextAttempt = now.Add(watchRetryDelay)
			continue
		}
		sent = candidate
	}
}

// sendCommand dispatches to the BLE or HTTP transport. command/args use
// the same shape either way: command is "color", "brightness", or "mode",
// and args is "r,g,b[,brightness]", a brightness level, or a mode name.
func sendCommand(apiKey, command, args string, wifi bool, host string, timeout time.Duration) error {
	if wifi {
		return sendHTTPCommand(apiKey, host, command, args, timeout)
	}
	return sendBLECommand(apiKey, command, args, timeout)
}

func sendHTTPCommand(apiKey, host, command, args string, timeout time.Duration) error {
	values := url.Values{}
	var path string

	switch command {
	case "color":
		parts := strings.Split(args, ",")
		if len(parts) < 3 {
			return fmt.Errorf("invalid color args %q", args)
		}
		values.Set("r", parts[0])
		values.Set("g", parts[1])
		values.Set("b", parts[2])
		if len(parts) >= 4 {
			values.Set("brightness", parts[3])
		}
		path = "/color"
	case "brightness":
		values.Set("level", args)
		path = "/brightness"
	case "mode":
		values.Set("name", args)
		path = "/mode"
	default:
		return fmt.Errorf("unknown command %q", command)
	}

	reqURL := fmt.Sprintf("http://%s%s?%s", host, path, values.Encode())
	req, err := http.NewRequest(http.MethodPost, reqURL, nil)
	if err != nil {
		return fmt.Errorf("building request failed: %w", err)
	}
	req.Header.Set("X-API-Key", apiKey)

	client := http.Client{Timeout: timeout}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("request to %s failed: %w", host, err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return fmt.Errorf("reading response failed: %w", err)
	}
	fmt.Println(string(body))
	return nil
}

func sendBLECommand(apiKey, command, args string, timeout time.Duration) error {
	if err := adapter.Enable(); err != nil {
		return fmt.Errorf("could not enable BLE adapter: %w", err)
	}

	device, err := findDevice(timeout)
	if err != nil {
		return err
	}
	defer device.Disconnect()

	services, err := device.DiscoverServices([]bluetooth.UUID{serviceUUID})
	if err != nil || len(services) == 0 {
		return fmt.Errorf("could not discover BLE service: %w", err)
	}

	chars, err := services[0].DiscoverCharacteristics([]bluetooth.UUID{commandCharUUID, statusCharUUID})
	if err != nil {
		return fmt.Errorf("could not discover characteristics: %w", err)
	}

	var commandChar, statusChar bluetooth.DeviceCharacteristic
	var haveCommand, haveStatus bool
	for _, c := range chars {
		switch c.UUID().String() {
		case commandCharUUID.String():
			commandChar, haveCommand = c, true
		case statusCharUUID.String():
			statusChar, haveStatus = c, true
		}
	}
	if !haveCommand || !haveStatus {
		return errors.New("device is missing the expected BLE characteristics")
	}

	payload := fmt.Sprintf("%s|%s|%s", apiKey, command, args)
	if _, err := commandChar.Write([]byte(payload)); err != nil {
		return fmt.Errorf("write failed: %w", err)
	}

	// Give the board a moment to process the command and update its status
	// characteristic before we read it back.
	time.Sleep(150 * time.Millisecond)

	buf := make([]byte, 256)
	n, err := statusChar.Read(buf)
	if err != nil {
		return fmt.Errorf("read failed: %w", err)
	}
	fmt.Println(string(buf[:n]))
	return nil
}

func findDevice(timeout time.Duration) (bluetooth.Device, error) {
	resultCh := make(chan bluetooth.ScanResult, 1)
	errCh := make(chan error, 1)

	// StopScan must run exactly once: a second call (from a duplicate
	// discovery event, or the timeout racing a match) blocks or panics.
	var stopOnce sync.Once
	stopScan := func() { stopOnce.Do(func() { adapter.StopScan() }) }

	go func() {
		err := adapter.Scan(func(a *bluetooth.Adapter, result bluetooth.ScanResult) {
			if result.LocalName() == deviceName {
				stopScan()
				select {
				case resultCh <- result:
				default:
				}
			}
		})
		if err != nil {
			errCh <- err
		}
	}()

	select {
	case result := <-resultCh:
		return adapter.Connect(result.Address, bluetooth.ConnectionParams{})
	case err := <-errCh:
		return bluetooth.Device{}, err
	case <-time.After(timeout):
		stopScan()
		return bluetooth.Device{}, fmt.Errorf(
			"could not find a BLE device named %q within %s; is it powered on and in range?",
			deviceName, timeout)
	}
}
