package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type state struct {
	RepoName      string `json:"repo_name"`
	RepoURL       string `json:"repo_url"`
	Protocol      string `json:"protocol"`
	LastSyncUTC   string `json:"last_sync_utc"`
	SyncStatus    string `json:"sync_status"`
	SyncOK        int    `json:"sync_ok"`
	PackageCount  int    `json:"package_count"`
	CloudProvider string `json:"cloud_provider"`
	CloudTarget   string `json:"cloud_target"`
	ShellBackend  string `json:"shell_backend"`
	FSBackend     string `json:"fs_backend"`
	TextEditor    string `json:"text_editor"`
	TaskManager   string `json:"task_manager"`
	FileManager   string `json:"file_manager"`
}

type manifest struct {
	RepoName      string     `json:"repo_name"`
	RepoURL       string     `json:"repo_url"`
	Packages      []pkgEntry `json:"packages"`
	CloudProvider string     `json:"cloud_provider"`
	CloudTarget   string     `json:"cloud_target"`
}

type pkgEntry struct {
	Name    string `json:"name"`
	Version string `json:"version"`
	URL     string `json:"url"`
	SHA256  string `json:"sha256,omitempty"`
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}

	switch os.Args[1] {
	case "manifest":
		runManifest(os.Args[2:])
	case "fetch":
		runFetch(os.Args[2:])
	case "sync":
		runSync(os.Args[2:])
	case "validate":
		runValidate(os.Args[2:])
	case "repo":
		runRepo(os.Args[2:])
	case "rest":
		runREST(os.Args[2:])
	default:
		usage()
		os.Exit(2)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "cosnet - HTTP/HTTPS, JSON, REST and repo/cloud sync helper")
	fmt.Fprintln(os.Stderr, "  manifest  Generate build/cosnet_state.h from defaults or JSON")
	fmt.Fprintln(os.Stderr, "  fetch     Download a URL to a file")
	fmt.Fprintln(os.Stderr, "  sync      Mirror a directory tree locally")
	fmt.Fprintln(os.Stderr, "  validate  Validate a JSON file")
	fmt.Fprintln(os.Stderr, "  repo      Fetch and cache a package repository manifest")
	fmt.Fprintln(os.Stderr, "  rest      Issue a simple REST request")
}

func defaultState() state {
	return state{
		RepoName:      "C-OS Package Repository",
		RepoURL:       "https://repo.example.invalid/cos",
		Protocol:      "HTTPS/JSON/REST",
		LastSyncUTC:   "never",
		SyncStatus:    "idle",
		SyncOK:        0,
		PackageCount:  0,
		CloudProvider: "local cache",
		CloudTarget:   "/storage/shared/repo-cache",
		ShellBackend:  "native shell",
		FSBackend:     "VFS + FAT32",
		TextEditor:    "C text editor",
		TaskManager:   "C task manager",
		FileManager:   "C file manager",
	}
}

func runManifest(args []string) {
	fs := flag.NewFlagSet("manifest", flag.ExitOnError)
	out := fs.String("out", "", "output header path")
	config := fs.String("config", "", "optional JSON config")
	fs.Parse(args)
	if *out == "" {
		fatalf("manifest: -out is required")
	}

	st := defaultState()
	if *config != "" {
		mf, err := loadManifest(*config)
		if err != nil {
			fatalf("manifest: load config: %v", err)
		}
		st = stateFromManifest(mf, st)
	}

	if err := writeHeader(*out, st); err != nil {
		fatalf("manifest: write header: %v", err)
	}
}

func runFetch(args []string) {
	fs := flag.NewFlagSet("fetch", flag.ExitOnError)
	url := fs.String("url", "", "HTTP/HTTPS URL")
	out := fs.String("out", "", "output file")
	method := fs.String("method", http.MethodGet, "HTTP method")
	bodyPath := fs.String("body", "", "optional body file")
	fs.Parse(args)
	if *url == "" || *out == "" {
		fatalf("fetch: -url and -out are required")
	}

	body, err := readOptionalFile(*bodyPath)
	if err != nil {
		fatalf("fetch: body: %v", err)
	}

	req, err := http.NewRequest(*method, *url, bytes.NewReader(body))
	if err != nil {
		fatalf("fetch: request: %v", err)
	}
	req.Header.Set("User-Agent", "cosnet/1.0")
	if len(body) > 0 {
		req.Header.Set("Content-Type", "application/json")
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		fatalf("fetch: do: %v", err)
	}
	defer resp.Body.Close()

	if err := os.MkdirAll(filepath.Dir(*out), 0o755); err != nil {
		fatalf("fetch: mkdir: %v", err)
	}
	f, err := os.Create(*out)
	if err != nil {
		fatalf("fetch: create: %v", err)
	}
	defer f.Close()
	if _, err := io.Copy(f, resp.Body); err != nil {
		fatalf("fetch: copy: %v", err)
	}
}

func runSync(args []string) {
	fs := flag.NewFlagSet("sync", flag.ExitOnError)
	src := fs.String("src", "", "source directory")
	dst := fs.String("dst", "", "destination directory")
	fs.Parse(args)
	if *src == "" || *dst == "" {
		fatalf("sync: -src and -dst are required")
	}
	if err := mirrorDir(*src, *dst); err != nil {
		fatalf("sync: %v", err)
	}
}

func runValidate(args []string) {
	fs := flag.NewFlagSet("validate", flag.ExitOnError)
	jsonPath := fs.String("json", "", "JSON file")
	fs.Parse(args)
	if *jsonPath == "" {
		fatalf("validate: -json is required")
	}
	data, err := os.ReadFile(*jsonPath)
	if err != nil {
		fatalf("validate: read: %v", err)
	}
	if !json.Valid(data) {
		fatalf("validate: invalid JSON")
	}
	fmt.Println("valid JSON")
}

func runRepo(args []string) {
	fs := flag.NewFlagSet("repo", flag.ExitOnError)
	url := fs.String("url", "", "repository manifest URL")
	out := fs.String("out", "", "cached manifest output")
	header := fs.String("header", "", "generated state header output")
	fs.Parse(args)
	if *url == "" || *out == "" {
		fatalf("repo: -url and -out are required")
	}

	mf, err := fetchManifest(*url)
	if err != nil {
		fatalf("repo: fetch manifest: %v", err)
	}
	if err := writeJSON(*out, mf); err != nil {
		fatalf("repo: write cache: %v", err)
	}

	st := defaultState()
	st.RepoName = firstNonEmpty(mf.RepoName, st.RepoName)
	st.RepoURL = firstNonEmpty(mf.RepoURL, *url)
	st.CloudProvider = firstNonEmpty(mf.CloudProvider, st.CloudProvider)
	st.CloudTarget = firstNonEmpty(mf.CloudTarget, st.CloudTarget)
	st.PackageCount = len(mf.Packages)
	st.SyncStatus = "ready"
	st.SyncOK = 1
	st.LastSyncUTC = time.Now().UTC().Format(time.RFC3339)
	if *header != "" {
		if err := writeHeader(*header, st); err != nil {
			fatalf("repo: write header: %v", err)
		}
	}
}

func runREST(args []string) {
	fs := flag.NewFlagSet("rest", flag.ExitOnError)
	method := fs.String("method", http.MethodGet, "HTTP method")
	url := fs.String("url", "", "request URL")
	bodyPath := fs.String("body", "", "request body file")
	headerKV := fs.String("header", "", "single header key:value")
	fs.Parse(args)
	if *url == "" {
		fatalf("rest: -url is required")
	}

	body, err := readOptionalFile(*bodyPath)
	if err != nil {
		fatalf("rest: body: %v", err)
	}
	req, err := http.NewRequest(*method, *url, bytes.NewReader(body))
	if err != nil {
		fatalf("rest: request: %v", err)
	}
	if *headerKV != "" {
		parts := strings.SplitN(*headerKV, ":", 2)
		if len(parts) == 2 {
			req.Header.Set(strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1]))
		}
	}
	if len(body) > 0 {
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("User-Agent", "cosnet/1.0")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		fatalf("rest: do: %v", err)
	}
	defer resp.Body.Close()
	out, err := io.ReadAll(resp.Body)
	if err != nil {
		fatalf("rest: read: %v", err)
	}
	fmt.Printf("status=%d bytes=%d\n", resp.StatusCode, len(out))
	os.Stdout.Write(out)
	if len(out) == 0 || out[len(out)-1] != '\n' {
		fmt.Println()
	}
}

func fetchManifest(url string) (*manifest, error) {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "cosnet/1.0")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("unexpected HTTP status %d", resp.StatusCode)
	}
	var mf manifest
	dec := json.NewDecoder(resp.Body)
	if err := dec.Decode(&mf); err != nil {
		return nil, err
	}
	if mf.RepoURL == "" {
		mf.RepoURL = url
	}
	return &mf, nil
}

func stateFromManifest(mf *manifest, st state) state {
	st.RepoName = firstNonEmpty(mf.RepoName, st.RepoName)
	st.RepoURL = firstNonEmpty(mf.RepoURL, st.RepoURL)
	st.CloudProvider = firstNonEmpty(mf.CloudProvider, st.CloudProvider)
	st.CloudTarget = firstNonEmpty(mf.CloudTarget, st.CloudTarget)
	st.PackageCount = len(mf.Packages)
	st.SyncStatus = "ready"
	st.SyncOK = 1
	st.LastSyncUTC = time.Now().UTC().Format(time.RFC3339)
	return st
}

func writeHeader(path string, st state) error {
	var b strings.Builder
	b.WriteString("#ifndef COSNET_STATE_H\n")
	b.WriteString("#define COSNET_STATE_H\n\n")
	b.WriteString("/* generated by tools/cosnet.go */\n")
	fmt.Fprintf(&b, "#define COSNET_REPO_NAME %s\n", cString(st.RepoName))
	fmt.Fprintf(&b, "#define COSNET_REPO_URL %s\n", cString(st.RepoURL))
	fmt.Fprintf(&b, "#define COSNET_REPO_PROTOCOL %s\n", cString(st.Protocol))
	fmt.Fprintf(&b, "#define COSNET_LAST_SYNC_UTC %s\n", cString(st.LastSyncUTC))
	fmt.Fprintf(&b, "#define COSNET_SYNC_STATUS %s\n", cString(st.SyncStatus))
	fmt.Fprintf(&b, "#define COSNET_SYNC_OK %d\n", st.SyncOK)
	fmt.Fprintf(&b, "#define COSNET_PACKAGE_COUNT %d\n", st.PackageCount)
	fmt.Fprintf(&b, "#define COSNET_CLOUD_PROVIDER %s\n", cString(st.CloudProvider))
	fmt.Fprintf(&b, "#define COSNET_CLOUD_TARGET %s\n", cString(st.CloudTarget))
	fmt.Fprintf(&b, "#define COSNET_SHELL_BACKEND %s\n", cString(st.ShellBackend))
	fmt.Fprintf(&b, "#define COSNET_FS_BACKEND %s\n", cString(st.FSBackend))
	fmt.Fprintf(&b, "#define COSNET_TEXT_EDITOR %s\n", cString(st.TextEditor))
	fmt.Fprintf(&b, "#define COSNET_TASK_MANAGER %s\n", cString(st.TaskManager))
	fmt.Fprintf(&b, "#define COSNET_FILE_MANAGER %s\n", cString(st.FileManager))
	b.WriteString("\n#endif /* COSNET_STATE_H */\n")
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

func cString(s string) string { return strconv.Quote(s) }

func writeJSON(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return os.WriteFile(path, data, 0o644)
}

func loadManifest(path string) (*manifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var mf manifest
	return &mf, json.Unmarshal(data, &mf)
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if strings.TrimSpace(v) != "" {
			return v
		}
	}
	return ""
}

func readOptionalFile(path string) ([]byte, error) {
	if path == "" {
		return nil, nil
	}
	return os.ReadFile(path)
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}

func mirrorDir(src, dst string) error {
	info, err := os.Stat(src)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("source is not a directory")
	}
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return err
	}
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return nil
		}
		target := filepath.Join(dst, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}
