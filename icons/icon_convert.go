package main

import (
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	// Find all PNG files in current directory
	files, err := filepath.Glob("*.png")
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error finding PNG files: %v\n", err)
		os.Exit(1)
	}

	if len(files) == 0 {
		fmt.Fprintf(os.Stderr, "No PNG files found\n")
		os.Exit(1)
	}

	// Process each PNG file
	for _, filename := range files {
		if err := convertPNGToArray(filename); err != nil {
			fmt.Fprintf(os.Stderr, "Error processing %s: %v\n", filename, err)
			continue
		}
	}
}

func convertPNGToArray(filename string) error {
	// Open PNG file
	file, err := os.Open(filename)
	if err != nil {
		return err
	}
	defer file.Close()

	// Decode PNG
	img, err := png.Decode(file)
	if err != nil {
		return err
	}

	bounds := img.Bounds()
	width := bounds.Dx()
	height := bounds.Dy()

	// Convert pixels to bits (transparent=0, opaque=1)
	bits := make([]byte, 0)
	var currentByte byte
	var bitCount int

	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			_, _, _, a := img.At(x, y).RGBA()

			// Shift current byte left and add new bit
			currentByte <<= 1

			// If pixel is opaque (alpha >= 50%), set bit to 1
			if a >= 0x8000 {
				currentByte |= 1
			}

			bitCount++

			// If we've filled a byte, add it to the slice
			if bitCount == 8 {
				bits = append(bits, currentByte)
				currentByte = 0
				bitCount = 0
			}
		}
	}

	// Handle remaining bits if total pixels not divisible by 8
	if bitCount > 0 {
		// Shift remaining bits to fill the byte
		currentByte <<= (8 - bitCount)
		bits = append(bits, currentByte)
	}

	// Generate array name from filename (without extension)
	arrayName := strings.TrimSuffix(filename, filepath.Ext(filename))

	// Output C array declaration
	fmt.Printf("const unsigned char %s[%d] = {\n", arrayName, len(bits))
	for i, b := range bits {
		if i%12 == 0 {
			fmt.Print("  ")
		}
		fmt.Printf("0x%02X", b)
		if i < len(bits)-1 {
			fmt.Print(",")
			if (i+1)%12 == 0 {
				fmt.Println()
			} else {
				fmt.Print(" ")
			}
		}
	}
	if len(bits)%12 != 0 {
		fmt.Println()
	}
	fmt.Printf("}; // %dx%d pixels\n\n", width, height)

	return nil
}