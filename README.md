# Image Swap

A simple command line tool for swapping image formats. 
Quick and basic. 

input / output determined by file extension.

## Formats

- JPEG (jpg, jpeg)
- PNG (png)
- WebP (webp)
- TIFF (tif, tiff)
- GIF (gif)
- HEIC/HEIF (heic, heif)

## Usage
- imgswap -q 100 input.ext output.ext
- imgswap -h

### Options

`-q <quality>`
    - Quality from 1 to 100
    - Only affects JPEG, WebP, and HEIC
    - Default is 90

- `-h`
    - Show usage

## Dependencies

- libjpeg, libpng, libwebp
- libtiff, giflib, libheif
