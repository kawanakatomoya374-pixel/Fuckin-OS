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
)

const (
	sectorSize = 512

	storageMagic       = 0x43535350464f5354 // "STOFCSSC" style tag
	storageVersion     = 4
	storageDiskSectors = 1048576 // 512 MiB

	storageCatalogSector   = 0
	storageCatalogSectors  = 64
	storageCatalogBackup   = 1 + storageCatalogSectors // 65
	storagePasswordSector  = storageCatalogBackup + storageCatalogSectors
	storageSettingsSector  = storagePasswordSector + 1
	storageReservedSector  = storageSettingsSector + 1
	storageDataStartSector = storageReservedSector + 1

	vfsMaxFiles   = 128
	vfsMaxNameLen = 128

	storageHeaderSize   = 8 * 8
	storageEntrySize    = 196
	storageCatalogSize  = storageHeaderSize + (vfsMaxFiles * storageEntrySize)
	storagePasswordSize = 8 + 32 + 16 + 8 + 8 + 8
)

var crcTable = crc32.MakeTable(crc32.IEEE)

func putU64(b []byte, off int, v uint64) {
	binary.LittleEndian.PutUint64(b[off:off+8], v)
}

func crc32Skip(data []byte, skipOff, skipLen int) uint64 {
	crc := uint32(0xFFFFFFFF)
	for i, v := range data {
		if i >= skipOff && i < skipOff+skipLen {
			continue
		}
		crc = crc32.Update(crc, crcTable, []byte{v})
	}
	return uint64(^crc)
}

func buildEmptyCatalog() []byte {
	buf := make([]byte, storageCatalogSize)
	// storage_header_t layout.
	putU64(buf, 0, storageMagic)
	putU64(buf, 8, storageVersion)
	putU64(buf, 16, storageDiskSectors)
	putU64(buf, 24, storageCatalogSectors)
	putU64(buf, 32, storageDataStartSector)
	putU64(buf, 40, vfsMaxFiles)
	putU64(buf, 48, 0) // used_files
	putU64(buf, 56, 0) // used_bytes
	putU64(buf, 64, 0) // checksum placeholder

	chk := crc32Skip(buf, 56, 8)
	putU64(buf, 64, chk)
	return buf
}

func buildPasswordRecord() []byte {
	buf := make([]byte, storagePasswordSize)
	putU64(buf, 0, storageMagic)
	// password_hash + salt + attempts + locked are already zeroed.
	putU64(buf, 56, 0)
	putU64(buf, 64, 0)
	putU64(buf, 72, 0)
	chk := crc32Skip(buf, 72, 8)
	putU64(buf, 72, chk)
	return buf
}

func writeSector(f *os.File, sector uint64, data []byte) error {
	if _, err := f.Seek(int64(sector*sectorSize), io.SeekStart); err != nil {
		return err
	}
	_, err := f.Write(data)
	return err
}

func ensureSize(path string, bytes int64) error {
	if bytes <= 0 {
		return errors.New("image size must be positive")
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	if err := f.Truncate(bytes); err != nil {
		return err
	}
	return nil
}

func buildImage(outPath string, sizeMB int) error {
	if sizeMB <= 0 {
		return errors.New("size-mb must be positive")
	}
	if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
		return err
	}
	targetBytes := int64(sizeMB) * 1024 * 1024
	if info, err := os.Stat(outPath); err == nil && info.Size() >= targetBytes {
		if err := verifyImage(outPath); err == nil {
			fmt.Printf("storage image preserved: %s (%d MiB)\n", outPath, sizeMB)
			return nil
		}
	}
	if err := ensureSize(outPath, targetBytes); err != nil {
		return err
	}

	f, err := os.OpenFile(outPath, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()

	catalog := buildEmptyCatalog()
	password := buildPasswordRecord()

	if err := writeSector(f, storageCatalogSector, catalog); err != nil {
		return fmt.Errorf("write primary catalog: %w", err)
	}
	if err := writeSector(f, storageCatalogBackup, catalog); err != nil {
		return fmt.Errorf("write backup catalog: %w", err)
	}
	if err := writeSector(f, storagePasswordSector, password); err != nil {
		return fmt.Errorf("write password sector: %w", err)
	}
	// Sector 130 and 131 remain zeroed by truncate; that is the intended empty state.
	return nil
}

func verifyImage(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		return err
	}
	if info.Size() < int64(storageDiskSectors*sectorSize) {
		return fmt.Errorf("image too small: got %d bytes", info.Size())
	}

	buf := make([]byte, storageCatalogSize)
	if _, err := f.ReadAt(buf, storageCatalogSector*sectorSize); err != nil {
		return fmt.Errorf("read primary catalog: %w", err)
	}
	if binary.LittleEndian.Uint64(buf[0:8]) != storageMagic {
		return errors.New("primary catalog magic mismatch")
	}
	if binary.LittleEndian.Uint64(buf[8:16]) != storageVersion {
		return errors.New("primary catalog version mismatch")
	}
	saved := binary.LittleEndian.Uint64(buf[64:72])
	putU64(buf, 64, 0)
	if saved != crc32Skip(buf, 56, 8) {
		return errors.New("primary catalog checksum mismatch")
	}

	if _, err := f.ReadAt(buf, storageCatalogBackup*sectorSize); err != nil {
		return fmt.Errorf("read backup catalog: %w", err)
	}
	if binary.LittleEndian.Uint64(buf[0:8]) != storageMagic {
		return errors.New("backup catalog magic mismatch")
	}
	saved = binary.LittleEndian.Uint64(buf[64:72])
	putU64(buf, 64, 0)
	if saved != crc32Skip(buf, 56, 8) {
		return errors.New("backup catalog checksum mismatch")
	}

	pwd := make([]byte, storagePasswordSize)
	if _, err := f.ReadAt(pwd, storagePasswordSector*sectorSize); err != nil {
		return fmt.Errorf("read password sector: %w", err)
	}
	if binary.LittleEndian.Uint64(pwd[0:8]) != storageMagic {
		return errors.New("password signature mismatch")
	}
	saved = binary.LittleEndian.Uint64(pwd[72:80])
	putU64(pwd, 72, 0)
	if saved != crc32Skip(pwd, 72, 8) {
		return errors.New("password checksum mismatch")
	}
	return nil
}

func main() {
	var (
		out    = flag.String("out", filepath.FromSlash("build/storage.img"), "output image path")
		sizeMB = flag.Int("size-mb", 512, "image size in MiB")
		verify = flag.String("verify", "", "verify an existing image instead of creating one")
	)
	flag.Parse()

	if *verify != "" {
		if err := verifyImage(*verify); err != nil {
			fmt.Fprintf(os.Stderr, "storage image verification failed: %v\n", err)
			os.Exit(1)
		}
		fmt.Println("storage image verification ok")
		return
	}

	if err := buildImage(*out, *sizeMB); err != nil {
		fmt.Fprintf(os.Stderr, "storage image build failed: %v\n", err)
		os.Exit(1)
	}
	if err := verifyImage(*out); err != nil {
		fmt.Fprintf(os.Stderr, "storage image post-check failed: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("storage image created: %s (%d MiB)\n", *out, *sizeMB)
}
