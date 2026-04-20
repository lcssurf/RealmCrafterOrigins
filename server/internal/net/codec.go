package net

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math"
)

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

// Writer builds a binary payload using little-endian encoding.
type Writer struct {
	buf bytes.Buffer
}

func (w *Writer) WriteUint8(v uint8) {
	w.buf.WriteByte(v)
}

func (w *Writer) WriteUint16(v uint16) {
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], v)
	w.buf.Write(b[:])
}

func (w *Writer) WriteUint32(v uint32) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	w.buf.Write(b[:])
}

func (w *Writer) WriteInt32(v int32) {
	w.WriteUint32(uint32(v))
}

func (w *Writer) WriteFloat32(v float32) {
	bits := math.Float32bits(v)
	w.WriteUint32(bits)
}

func (w *Writer) WriteBool(v bool) {
	if v {
		w.buf.WriteByte(1)
	} else {
		w.buf.WriteByte(0)
	}
}

// WriteString writes a uint16 length-prefixed UTF-8 string.
func (w *Writer) WriteString(s string) {
	b := []byte(s)
	w.WriteUint16(uint16(len(b)))
	w.buf.Write(b)
}

// WriteRaw appends raw bytes directly to the buffer without any length prefix.
func (w *Writer) WriteRaw(data []byte) {
	w.buf.Write(data)
}

// Bytes returns the accumulated payload bytes.
func (w *Writer) Bytes() []byte {
	return w.buf.Bytes()
}

// Reset clears the internal buffer for reuse.
func (w *Writer) Reset() {
	w.buf.Reset()
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

// Reader reads a binary payload using little-endian encoding.
type Reader struct {
	r *bytes.Reader
}

// NewReader wraps a payload slice in a Reader.
func NewReader(data []byte) *Reader {
	return &Reader{r: bytes.NewReader(data)}
}

func (r *Reader) ReadUint8() (uint8, error) {
	b, err := r.r.ReadByte()
	if err != nil {
		return 0, fmt.Errorf("ReadUint8: %w", err)
	}
	return b, nil
}

func (r *Reader) ReadUint16() (uint16, error) {
	var b [2]byte
	if _, err := io.ReadFull(r.r, b[:]); err != nil {
		return 0, fmt.Errorf("ReadUint16: %w", err)
	}
	return binary.LittleEndian.Uint16(b[:]), nil
}

func (r *Reader) ReadUint32() (uint32, error) {
	var b [4]byte
	if _, err := io.ReadFull(r.r, b[:]); err != nil {
		return 0, fmt.Errorf("ReadUint32: %w", err)
	}
	return binary.LittleEndian.Uint32(b[:]), nil
}

func (r *Reader) ReadInt32() (int32, error) {
	v, err := r.ReadUint32()
	return int32(v), err
}

func (r *Reader) ReadFloat32() (float32, error) {
	bits, err := r.ReadUint32()
	if err != nil {
		return 0, fmt.Errorf("ReadFloat32: %w", err)
	}
	return math.Float32frombits(bits), nil
}

func (r *Reader) ReadBool() (bool, error) {
	b, err := r.ReadUint8()
	if err != nil {
		return false, err
	}
	return b != 0, nil
}

// ReadString reads a uint16 length-prefixed UTF-8 string.
func (r *Reader) ReadString() (string, error) {
	length, err := r.ReadUint16()
	if err != nil {
		return "", fmt.Errorf("ReadString length: %w", err)
	}
	if length == 0 {
		return "", nil
	}
	buf := make([]byte, length)
	if _, err := io.ReadFull(r.r, buf); err != nil {
		return "", fmt.Errorf("ReadString body: %w", err)
	}
	return string(buf), nil
}

// ---------------------------------------------------------------------------
// Framing helpers
// ---------------------------------------------------------------------------

// WritePacket writes a fully framed packet to w:
//
//	[uint16 type LE][uint32 payloadLen LE][payload...]
func WritePacket(w io.Writer, pktType uint16, payload []byte) error {
	var hdr [6]byte
	binary.LittleEndian.PutUint16(hdr[0:2], pktType)
	binary.LittleEndian.PutUint32(hdr[2:6], uint32(len(payload)))
	if _, err := w.Write(hdr[:]); err != nil {
		return fmt.Errorf("WritePacket header: %w", err)
	}
	if len(payload) > 0 {
		if _, err := w.Write(payload); err != nil {
			return fmt.Errorf("WritePacket payload: %w", err)
		}
	}
	return nil
}

// ReadPacket reads a fully framed packet from r.
func ReadPacket(r io.Reader) (pktType uint16, payload []byte, err error) {
	var hdr [6]byte
	if _, err = io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, fmt.Errorf("ReadPacket header: %w", err)
	}
	pktType = binary.LittleEndian.Uint16(hdr[0:2])
	payloadLen := binary.LittleEndian.Uint32(hdr[2:6])
	if payloadLen == 0 {
		return pktType, nil, nil
	}
	payload = make([]byte, payloadLen)
	if _, err = io.ReadFull(r, payload); err != nil {
		return 0, nil, fmt.Errorf("ReadPacket payload: %w", err)
	}
	return pktType, payload, nil
}
