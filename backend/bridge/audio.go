package main

import (
	"encoding/binary"
	"math"
)

// pcm16ToFloat32 converts 16-bit signed integer little-endian PCM bytes to float32 slice [-1.0, 1.0].
func pcm16ToFloat32(pcm []byte) []float32 {
	n := len(pcm) / 2
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		v := int16(binary.LittleEndian.Uint16(pcm[i*2:]))
		out[i] = float32(v) / 32768.0
	}
	return out
}

// float32ToPCM16 converts float32 slice [-1.0, 1.0] to 16-bit signed integer little-endian PCM bytes.
func float32ToPCM16(samples []float32) []byte {
	out := make([]byte, len(samples)*2)
	for i, s := range samples {
		clamped := math.Max(-1.0, math.Min(1.0, float64(s)))
		v := int16(clamped * 32767.0)
		binary.LittleEndian.PutUint16(out[i*2:], uint16(v))
	}
	return out
}

// resampleLinear resamples float32 audio between arbitrary sample rates.
func resampleLinear(input []float32, fromRate, toRate int) []float32 {
	if fromRate == toRate || len(input) == 0 {
		return input
	}
	ratio := float64(fromRate) / float64(toRate)
	outLen := int(float64(len(input)) / ratio)
	if outLen <= 0 {
		return []float32{}
	}
	out := make([]float32, outLen)
	for i := 0; i < outLen; i++ {
		pos := float64(i) * ratio
		idx := int(pos)
		frac := float32(pos - float64(idx))
		if idx+1 < len(input) {
			out[i] = input[idx]*(1.0-frac) + input[idx+1]*frac
		} else if idx < len(input) {
			out[i] = input[idx]
		}
	}
	return out
}

// resample22to16 resamples audio from 22,050 Hz to 16,000 Hz.
func resample22to16(input []float32) []float32 {
	return resampleLinear(input, 22050, 16000)
}
