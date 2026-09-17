package main

import (
    "encoding/binary"
    "errors"
    "flag"
    "fmt"
    "hash/crc32"
    "io"
    "os"
    "path/filepath"
    "strings"
    "time"
)

const (
    sectorSize = 512
    storageMagic uint64 = 0x43535350464f5354
    storageVersion uint64 = 4
    diskSectors uint64 = 1048576
    catalogSector uint64 = 0
    catalogSectors uint64 = 64
    backupCatalogSector uint64 = 65
    passwordSector uint64 = 129
    settingsSector uint64 = 130
    reservedSector uint64 = 131
    dataStartSector uint64 = 132
    metaRingSectors uint64 = 4096
    maxFiles = 128
    maxPath = 128
    headerSize = 72
    entrySize = 188
    catalogSize = headerSize + maxFiles*entrySize
    checksumOffset = 64
    entryChecksumOffset = 180
)

var crcTable = crc32.MakeTable(crc32.IEEE)

type addList []string
func (a *addList) String() string { return strings.Join(*a, ",") }
func (a *addList) Set(v string) error { *a = append(*a, v); return nil }

func u64(b []byte, off int) uint64 { return binary.LittleEndian.Uint64(b[off:off+8]) }
func put64(b []byte, off int, v uint64) { binary.LittleEndian.PutUint64(b[off:off+8], v) }
func checksumSkip(data []byte, skip, length int) uint64 {
    crc := uint32(0xffffffff)
    for i, x := range data {
        if i >= skip && i < skip+length { continue }
        crc = crc32.Update(crc, crcTable, []byte{x})
    }
    return uint64(^crc)
}
func pathBytes(dst []byte, path string) {
    for i := range dst { dst[i] = 0 }
    path = strings.ReplaceAll(path, "\\", "/")
    if !strings.HasPrefix(path, "/") { path = "/" + path }
    if len(path) >= len(dst) { path = path[:len(dst)-1] }
    copy(dst, path)
}
func entryPath(e []byte) string { return strings.TrimRight(string(e[52:180]), "\x00") }
func entryChecksum(e []byte) uint64 { return checksumSkip(e, entryChecksumOffset, 8) }
func catalogChecksum(c []byte) uint64 { return checksumSkip(c, checksumOffset, 8) }
func readAtFull(f *os.File, off int64, dst []byte) error {
    n, err := f.ReadAt(dst, off)
    if err != nil && !(err == io.EOF && n == len(dst)) { return err }
    if n != len(dst) { return io.ErrUnexpectedEOF }
    return nil
}
func writeAtFull(f *os.File, off int64, src []byte) error {
    n, err := f.WriteAt(src, off)
    if err != nil { return err }
    if n != len(src) { return io.ErrShortWrite }
    return nil
}
func fail(format string, args ...any) error { return fmt.Errorf(format, args...) }

func loadCatalog(f *os.File) ([]byte, error) {
    c := make([]byte, catalogSize)
    if err := readAtFull(f, int64(catalogSector*sectorSize), c); err != nil { return nil, err }
    if u64(c, 0) != storageMagic || u64(c, 8) != storageVersion ||
       u64(c, 16) != diskSectors || u64(c, 24) != catalogSectors ||
       u64(c, 32) != dataStartSector || u64(c, 40) != maxFiles {
        return nil, errors.New("storage catalog header mismatch")
    }
    saved := u64(c, checksumOffset); put64(c, checksumOffset, 0)
    if saved != catalogChecksum(c) { return nil, errors.New("storage catalog checksum mismatch") }
    put64(c, checksumOffset, saved)
    return c, nil
}
func findEntry(c []byte, path string) int {
    for i := 0; i < maxFiles; i++ {
        e := c[headerSize+i*entrySize:headerSize+(i+1)*entrySize]
        if e[0] != 0 && entryPath(e) == path { return i }
    }
    return -1
}
func usedEntryCount(c []byte) uint64 {
    var n uint64
    for i := 0; i < maxFiles; i++ { if c[headerSize+i*entrySize] != 0 { n++ } }
    return n
}
func ensureDir(c []byte, path string, now uint64) error {
    if path == "" || path == "/" { return nil }
    if findEntry(c, path) >= 0 { return nil }
    parent := filepath.ToSlash(filepath.Dir(path)); if parent == "." { parent = "/" }
    if err := ensureDir(c, parent, now); err != nil { return err }
    slot := -1
    for i := 0; i < maxFiles; i++ { if c[headerSize+i*entrySize] == 0 { slot = i; break } }
    if slot < 0 { return errors.New("storage catalog has no free entry for directory") }
    e := c[headerSize+slot*entrySize:headerSize+(slot+1)*entrySize]
    for i := range e { e[i] = 0 }
    e[0], e[1] = 1, 1
    put64(e, 28, now); put64(e, 36, now); put64(e, 44, now)
    pathBytes(e[52:180], path); put64(e, entryChecksumOffset, entryChecksum(e))
    return nil
}
func nextDataSector(c []byte) uint64 {
    next := dataStartSector
    for i := 0; i < maxFiles; i++ {
        e := c[headerSize+i*entrySize:headerSize+(i+1)*entrySize]
        if e[0] == 0 || e[1] != 0 { continue }
        end := u64(e, 4) + u64(e, 12)
        if end > next { next = end }
    }
    return next
}
func inject(image string, additions []string) error {
    f, err := os.OpenFile(image, os.O_RDWR, 0)
    if err != nil { return err }
    defer f.Close()
    info, err := f.Stat(); if err != nil { return err }
    if info.Size() < int64(diskSectors*sectorSize) { return fail("image too small: %d bytes", info.Size()) }
    c, err := loadCatalog(f); if err != nil { return err }
    now := uint64(time.Now().Unix())
    next := nextDataSector(c)
    var addedBytes uint64
    for _, spec := range additions {
        parts := strings.SplitN(spec, "=", 2)
        if len(parts) != 2 { return fail("--add expects host-file=/path/in/C-OS: %s", spec) }
        host, dest := parts[0], strings.ReplaceAll(parts[1], "\\", "/")
        if !strings.HasPrefix(dest, "/") { dest = "/" + dest }
        data, err := os.ReadFile(host); if err != nil { return err }
        if len(data) == 0 { return fail("empty file is not useful for test injection: %s", host) }
        if uint64(len(data)) > (diskSectors-metaRingSectors-dataStartSector)*sectorSize { return fail("file does not fit: %s", host) }
        if err := ensureDir(c, filepath.ToSlash(filepath.Dir(dest)), now); err != nil { return err }
        idx := findEntry(c, dest)
        if idx < 0 {
            for i := 0; i < maxFiles; i++ { if c[headerSize+i*entrySize] == 0 { idx = i; break } }
        }
        if idx < 0 { return errors.New("storage catalog has no free file entry") }
        e := c[headerSize+idx*entrySize:headerSize+(idx+1)*entrySize]
        for i := range e { e[i] = 0 }
        sectors := (uint64(len(data)) + sectorSize - 1) / sectorSize
        if next+sectors >= diskSectors-metaRingSectors { return errors.New("storage data area is full") }
        e[0], e[1] = 1, 0; put64(e, 4, next); put64(e, 12, sectors); put64(e, 20, uint64(len(data)))
        put64(e, 28, now); put64(e, 36, now); put64(e, 44, now); pathBytes(e[52:180], dest)
        put64(e, entryChecksumOffset, entryChecksum(e))
        padded := make([]byte, int(sectors)*sectorSize); copy(padded, data)
        if err := writeAtFull(f, int64(next*sectorSize), padded); err != nil { return err }
        fmt.Printf("injected %s -> %s (%d bytes, sectors %d)\n", host, dest, len(data), sectors)
        next += sectors; addedBytes += uint64(len(data))
    }
    put64(c, 48, usedEntryCount(c)); put64(c, 56, u64(c, 56)+addedBytes)
    put64(c, checksumOffset, 0); put64(c, checksumOffset, catalogChecksum(c))
    if err := writeAtFull(f, int64(catalogSector*sectorSize), c); err != nil { return err }
    if err := writeAtFull(f, int64(backupCatalogSector*sectorSize), c); err != nil { return err }
    verify, err := loadCatalog(f); if err != nil { return err }
    if u64(verify, 48) != usedEntryCount(verify) { return errors.New("post-write catalog verification failed") }
    fmt.Printf("storage injection verified: %s (%d entries)\n", image, u64(verify, 48))
    return nil
}

func main() {
    image := flag.String("image", "build/storage.img", "C-OS raw storage image")
    var additions addList
    flag.Var(&additions, "add", "host-file=/path/in/C-OS (repeatable)")
    flag.Parse()
    if len(additions) == 0 { fmt.Fprintln(os.Stderr, "usage: inject_storage -image build/storage.img -add sample.mp3=/music/sample.mp3"); os.Exit(2) }
    if err := inject(*image, additions); err != nil { fmt.Fprintln(os.Stderr, "storage injection failed:", err); os.Exit(1) }
}
