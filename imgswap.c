#include <ctype.h>
#include <errno.h>
#include <gif_lib.h>
#include <libheif/heif.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <tiffio.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

typedef enum {
    FORMAT_UNKNOWN = 0,
    FORMAT_JPEG,
    FORMAT_PNG,
    FORMAT_WEBP,
    FORMAT_TIFF,
    FORMAT_GIF,
    FORMAT_HEIC
} ImageFormat;

typedef struct {
    int width;
    int height;
    unsigned char *rgba;
} Image;

static void print_usage(const char *program_name) {

    fprintf(stderr, "supported formats: jpg jpeg png webp tif tiff gif heic heif\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "usage: %s [-q quality] [-r WIDTHxHEIGHT] [-s] input output\n", program_name);
    fprintf(stderr, "       %s -h\n", program_name);
    fprintf(stderr, "\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "quality range: 1-100 (used by jpg webp heic output)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "-r, --resize WxH   resize output image to exact width x height\n");
    fprintf(stderr, "-s, --shrink       reduce output file size using more aggressive compression\n");
}

static int parse_quality(const char *value, int *quality_out) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);

    if (*value == '\0' || *end != '\0' || parsed < 1 || parsed > 100) {
        return 0;
    }

    *quality_out = (int)parsed;
    return 1;
}

static int parse_resize(const char *value, int *width_out, int *height_out) {
    char *x_pos;
    char *end_w = NULL;
    char *end_h = NULL;
    long w;
    long h;

    if (!value || *value == '\0') {
        return 0;
    }

    x_pos = strchr(value, 'x');
    if (!x_pos) {
        x_pos = strchr(value, 'X');
    }
    if (!x_pos) {
        return 0;
    }

    w = strtol(value, &end_w, 10);
    h = strtol(x_pos + 1, &end_h, 10);

    if (end_w != x_pos || *end_h != '\0' || w <= 0 || h <= 0 || w > 100000 || h > 100000) {
        return 0;
    }

    *width_out = (int)w;
    *height_out = (int)h;
    return 1;
}

static ImageFormat format_from_path(const char *path) {
    const char *dot = strrchr(path, '.');

    if (!dot || *(dot + 1) == '\0') {
        return FORMAT_UNKNOWN;
    }

    dot++;
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) {
        return FORMAT_JPEG;
    }
    if (strcasecmp(dot, "png") == 0) {
        return FORMAT_PNG;
    }
    if (strcasecmp(dot, "webp") == 0) {
        return FORMAT_WEBP;
    }
    if (strcasecmp(dot, "tif") == 0 || strcasecmp(dot, "tiff") == 0) {
        return FORMAT_TIFF;
    }
    if (strcasecmp(dot, "gif") == 0) {
        return FORMAT_GIF;
    }
    if (strcasecmp(dot, "heic") == 0 || strcasecmp(dot, "heif") == 0) {
        return FORMAT_HEIC;
    }

    return FORMAT_UNKNOWN;
}

static void image_free(Image *img) {
    if (img && img->rgba) {
        free(img->rgba);
        img->rgba = NULL;
    }
    if (img) {
        img->width = 0;
        img->height = 0;
    }
}

static int alpha_blend_white(unsigned char c, unsigned char a) {
    return ((int)c * (int)a + 255 * (255 - (int)a)) / 255;
}

static void print_heif_error(const char *op, struct heif_error err) {
    fprintf(stderr, "%s: %s\n", op, err.message ? err.message : "unknown error");
}

static int resize_image(const Image *src, Image *dst, int new_width, int new_height) {
    int x;
    int y;

    if (!src || !src->rgba || new_width <= 0 || new_height <= 0) {
        return 0;
    }

    dst->width = new_width;
    dst->height = new_height;
    dst->rgba = malloc((size_t)new_width * (size_t)new_height * 4u);
    if (!dst->rgba) {
        fprintf(stderr, "resize: out of memory\n");
        return 0;
    }

    for (y = 0; y < new_height; ++y) {
        int src_y = (int)((long long)y * src->height / new_height);
        if (src_y >= src->height) {
            src_y = src->height - 1;
        }

        for (x = 0; x < new_width; ++x) {
            int src_x = (int)((long long)x * src->width / new_width);
            size_t src_idx;
            size_t dst_idx;

            if (src_x >= src->width) {
                src_x = src->width - 1;
            }

            src_idx = ((size_t)src_y * (size_t)src->width + (size_t)src_x) * 4u;
            dst_idx = ((size_t)y * (size_t)new_width + (size_t)x) * 4u;

            dst->rgba[dst_idx + 0] = src->rgba[src_idx + 0];
            dst->rgba[dst_idx + 1] = src->rgba[src_idx + 1];
            dst->rgba[dst_idx + 2] = src->rgba[src_idx + 2];
            dst->rgba[dst_idx + 3] = src->rgba[src_idx + 3];
        }
    }

    return 1;
}

static int decode_jpeg(const char *path, Image *out) {
    FILE *fp = fopen(path, "rb");
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY buffer = NULL;
    int row_stride;
    int y;

    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        fprintf(stderr, "decode jpeg: invalid header\n");
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return 0;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    out->width = (int)cinfo.output_width;
    out->height = (int)cinfo.output_height;
    out->rgba = malloc((size_t)out->width * (size_t)out->height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode jpeg: out of memory\n");
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return 0;
    }

    row_stride = out->width * 3;
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, (JDIMENSION)row_stride, 1);

    y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        int x;
        jpeg_read_scanlines(&cinfo, buffer, 1);
        for (x = 0; x < out->width; ++x) {
            size_t src_idx = (size_t)x * 3u;
            size_t dst_idx = ((size_t)y * (size_t)out->width + (size_t)x) * 4u;
            out->rgba[dst_idx + 0] = buffer[0][src_idx + 0];
            out->rgba[dst_idx + 1] = buffer[0][src_idx + 1];
            out->rgba[dst_idx + 2] = buffer[0][src_idx + 2];
            out->rgba[dst_idx + 3] = 255;
        }
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
}

static int encode_jpeg(const char *path, const Image *img, int quality, int shrink) {
    FILE *fp = fopen(path, "wb");
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    unsigned char *row = NULL;
    int x;
    int effective_quality = quality;

    if (shrink && effective_quality > 75) {
        effective_quality = 75;
    }

    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = (JDIMENSION)img->width;
    cinfo.image_height = (JDIMENSION)img->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, effective_quality, TRUE);

    if (shrink) {
        cinfo.optimize_coding = TRUE;
        cinfo.dct_method = JDCT_FASTEST;
    }

    jpeg_start_compress(&cinfo, TRUE);

    row = malloc((size_t)img->width * 3u);
    if (!row) {
        fprintf(stderr, "encode jpeg: out of memory\n");
        jpeg_destroy_compress(&cinfo);
        fclose(fp);
        return 0;
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        size_t y = (size_t)cinfo.next_scanline;
        for (x = 0; x < img->width; ++x) {
            size_t src = (y * (size_t)img->width + (size_t)x) * 4u;
            size_t dst = (size_t)x * 3u;
            row[dst + 0] = img->rgba[src + 0];
            row[dst + 1] = img->rgba[src + 1];
            row[dst + 2] = img->rgba[src + 2];
        }
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 1;
}

static int decode_png(const char *path, Image *out) {
    FILE *fp = fopen(path, "rb");
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep *rows = NULL;
    png_uint_32 w;
    png_uint_32 h;
    int bit_depth;
    int color_type;
    size_t y;

    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct(png);
    if (!png || !info) {
        fprintf(stderr, "decode png: init failed\n");
        if (png) {
            png_destroy_read_struct(&png, &info, NULL);
        }
        fclose(fp);
        return 0;
    }

    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "decode png: read failed\n");
        if (rows) {
            free(rows);
        }
        image_free(out);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    w = png_get_image_width(png, info);
    h = png_get_image_height(png, info);
    bit_depth = png_get_bit_depth(png, info);
    color_type = png_get_color_type(png, info);

    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);

    out->width = (int)w;
    out->height = (int)h;
    out->rgba = malloc((size_t)out->width * (size_t)out->height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode png: out of memory\n");
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }

    rows = malloc((size_t)h * sizeof(*rows));
    if (!rows) {
        fprintf(stderr, "decode png: out of memory\n");
        image_free(out);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return 0;
    }

    for (y = 0; y < (size_t)h; ++y) {
        rows[y] = out->rgba + y * (size_t)w * 4u;
    }

    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 1;
}

static int encode_png(const char *path, const Image *img, int shrink) {
    FILE *fp = fopen(path, "wb");
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep *rows = NULL;
    size_t y;

    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct(png);
    if (!png || !info) {
        fprintf(stderr, "encode png: init failed\n");
        if (png) {
            png_destroy_write_struct(&png, &info);
        }
        fclose(fp);
        return 0;
    }

    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "encode png: write failed\n");
        free(rows);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }

    png_init_io(png, fp);

    if (shrink) {
        png_set_compression_level(png, 9);
        png_set_compression_mem_level(png, 9);
        png_set_compression_strategy(png, PNG_Z_DEFAULT_STRATEGY);
        png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);
    }

    png_set_IHDR(
        png,
        info,
        (png_uint_32)img->width,
        (png_uint_32)img->height,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    rows = malloc((size_t)img->height * sizeof(*rows));
    if (!rows) {
        fprintf(stderr, "encode png: out of memory\n");
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return 0;
    }

    for (y = 0; y < (size_t)img->height; ++y) {
        rows[y] = (png_bytep)(img->rgba + y * (size_t)img->width * 4u);
    }

    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

static int decode_webp(const char *path, Image *out) {
    FILE *fp = fopen(path, "rb");
    long size;
    uint8_t *input = NULL;
    uint8_t *rgba = NULL;
    int width;
    int height;

    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    size = ftell(fp);
    if (size <= 0) {
        fprintf(stderr, "decode webp: empty input\n");
        fclose(fp);
        return 0;
    }
    rewind(fp);

    input = malloc((size_t)size);
    if (!input) {
        fprintf(stderr, "decode webp: out of memory\n");
        fclose(fp);
        return 0;
    }

    if (fread(input, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "decode webp: read failed\n");
        free(input);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    rgba = WebPDecodeRGBA(input, (size_t)size, &width, &height);
    free(input);
    if (!rgba) {
        fprintf(stderr, "decode webp: invalid data\n");
        return 0;
    }

    out->width = width;
    out->height = height;
    out->rgba = malloc((size_t)width * (size_t)height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode webp: out of memory\n");
        WebPFree(rgba);
        return 0;
    }
    memcpy(out->rgba, rgba, (size_t)width * (size_t)height * 4u);
    WebPFree(rgba);
    return 1;
}

static int encode_webp(const char *path, const Image *img, int quality, int shrink) {
    uint8_t *out_data = NULL;
    size_t out_size;
    FILE *fp;
    float effective_quality = (float)quality;

    if (shrink && effective_quality > 70.0f) {
        effective_quality = 70.0f;
    }

    out_size = WebPEncodeRGBA(
        img->rgba,
        img->width,
        img->height,
        img->width * 4,
        effective_quality,
        &out_data
    );

    if (out_size == 0 || !out_data) {
        fprintf(stderr, "encode webp: failed\n");
        return 0;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        WebPFree(out_data);
        return 0;
    }

    if (fwrite(out_data, 1, out_size, fp) != out_size) {
        fprintf(stderr, "encode webp: write failed\n");
        fclose(fp);
        WebPFree(out_data);
        return 0;
    }

    fclose(fp);
    WebPFree(out_data);
    return 1;
}

static int decode_tiff(const char *path, Image *out) {
    TIFF *tif = TIFFOpen(path, "r");
    uint32_t width;
    uint32_t height;
    uint32_t *raster = NULL;
    size_t i;

    if (!tif) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height)) {
        fprintf(stderr, "decode tiff: missing dimensions\n");
        TIFFClose(tif);
        return 0;
    }

    raster = _TIFFmalloc((size_t)width * (size_t)height * sizeof(*raster));
    if (!raster) {
        fprintf(stderr, "decode tiff: out of memory\n");
        TIFFClose(tif);
        return 0;
    }

    if (!TIFFReadRGBAImageOriented(tif, width, height, raster, ORIENTATION_TOPLEFT, 0)) {
        fprintf(stderr, "decode tiff: read failed\n");
        _TIFFfree(raster);
        TIFFClose(tif);
        return 0;
    }

    out->width = (int)width;
    out->height = (int)height;
    out->rgba = malloc((size_t)out->width * (size_t)out->height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode tiff: out of memory\n");
        _TIFFfree(raster);
        TIFFClose(tif);
        return 0;
    }

    for (i = 0; i < (size_t)width * (size_t)height; ++i) {
        uint32_t p = raster[i];
        out->rgba[i * 4u + 0] = TIFFGetR(p);
        out->rgba[i * 4u + 1] = TIFFGetG(p);
        out->rgba[i * 4u + 2] = TIFFGetB(p);
        out->rgba[i * 4u + 3] = TIFFGetA(p);
    }

    _TIFFfree(raster);
    TIFFClose(tif);
    return 1;
}

static int encode_tiff(const char *path, const Image *img, int shrink) {
    TIFF *tif = TIFFOpen(path, "w");
    uint16_t extrasamples = EXTRASAMPLE_ASSOCALPHA;
    int y;

    if (!tif) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, img->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, img->height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);

    if (shrink) {
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
        TIFFSetField(tif, TIFFTAG_PREDICTOR, 2);
    } else {
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    }

    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extrasamples);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, (uint32_t)-1));

    for (y = 0; y < img->height; ++y) {
        unsigned char *row = img->rgba + (size_t)y * (size_t)img->width * 4u;
        if (TIFFWriteScanline(tif, row, (uint32_t)y, 0) < 0) {
            fprintf(stderr, "encode tiff: write failed\n");
            TIFFClose(tif);
            return 0;
        }
    }

    TIFFClose(tif);
    return 1;
}

static int gif_transparent_index(const SavedImage *frame) {
    int i;

    for (i = 0; i < frame->ExtensionBlockCount; ++i) {
        const ExtensionBlock *ext = &frame->ExtensionBlocks[i];
        if (ext->Function == GRAPHICS_EXT_FUNC_CODE && ext->ByteCount >= 4) {
            int packed = ext->Bytes[0] & 0xFF;
            if (packed & 0x01) {
                return ext->Bytes[3] & 0xFF;
            }
        }
    }

    return -1;
}

static int decode_gif(const char *path, Image *out) {
    int err = 0;
    GifFileType *gif = DGifOpenFileName(path, &err);
    SavedImage *frame;
    ColorMapObject *cmap;
    int transparent;
    int x;
    int y;

    if (!gif) {
        fprintf(stderr, "decode gif: open failed\n");
        return 0;
    }

    if (DGifSlurp(gif) != GIF_OK || gif->ImageCount < 1) {
        fprintf(stderr, "decode gif: read failed\n");
        DGifCloseFile(gif, &err);
        return 0;
    }

    frame = &gif->SavedImages[0];
    cmap = frame->ImageDesc.ColorMap ? frame->ImageDesc.ColorMap : gif->SColorMap;
    if (!cmap) {
        fprintf(stderr, "decode gif: missing color map\n");
        DGifCloseFile(gif, &err);
        return 0;
    }

    transparent = gif_transparent_index(frame);

    out->width = frame->ImageDesc.Width;
    out->height = frame->ImageDesc.Height;
    out->rgba = malloc((size_t)out->width * (size_t)out->height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode gif: out of memory\n");
        DGifCloseFile(gif, &err);
        return 0;
    }

    for (y = 0; y < out->height; ++y) {
        for (x = 0; x < out->width; ++x) {
            int idx = frame->RasterBits[(size_t)y * (size_t)out->width + (size_t)x] & 0xFF;
            size_t dst = ((size_t)y * (size_t)out->width + (size_t)x) * 4u;
            GifColorType color;

            if (idx >= cmap->ColorCount) {
                color.Red = 0;
                color.Green = 0;
                color.Blue = 0;
            } else {
                color = cmap->Colors[idx];
            }

            out->rgba[dst + 0] = color.Red;
            out->rgba[dst + 1] = color.Green;
            out->rgba[dst + 2] = color.Blue;
            out->rgba[dst + 3] = (idx == transparent) ? 0 : 255;
        }
    }

    DGifCloseFile(gif, &err);
    return 1;
}

static int encode_gif(const char *path, const Image *img) {
    int err = 0;
    GifFileType *gif = NULL;
    GifByteType *indexed = NULL;
    GifColorType *palette = NULL;
    ColorMapObject *cmap = NULL;
    int color_count = 256;
    int y;
    size_t pixels = (size_t)img->width * (size_t)img->height;

    indexed = malloc(pixels);
    palette = calloc((size_t)color_count, sizeof(*palette));
    if (!indexed || !palette) {
        fprintf(stderr, "encode gif: out of memory\n");
        free(indexed);
        free(palette);
        return 0;
    }

    {
        int ri;
        int gi;
        int bi;
        int idx = 0;

        for (ri = 0; ri < 6; ++ri) {
            for (gi = 0; gi < 6; ++gi) {
                for (bi = 0; bi < 6; ++bi) {
                    palette[idx].Red = (GifByteType)(ri * 51);
                    palette[idx].Green = (GifByteType)(gi * 51);
                    palette[idx].Blue = (GifByteType)(bi * 51);
                    idx++;
                }
            }
        }

        while (idx < color_count) {
            int gray = (idx - 216) * 255 / (color_count - 216 - 1);
            palette[idx].Red = (GifByteType)gray;
            palette[idx].Green = (GifByteType)gray;
            palette[idx].Blue = (GifByteType)gray;
            idx++;
        }
    }

    for (y = 0; y < img->height; ++y) {
        int x;
        for (x = 0; x < img->width; ++x) {
            size_t i = (size_t)y * (size_t)img->width + (size_t)x;
            size_t src = i * 4u;
            int r;
            int g;
            int b;
            int ri;
            int gi;
            int bi;
            unsigned char a = img->rgba[src + 3];

            r = alpha_blend_white(img->rgba[src + 0], a);
            g = alpha_blend_white(img->rgba[src + 1], a);
            b = alpha_blend_white(img->rgba[src + 2], a);

            ri = r * 5 / 255;
            gi = g * 5 / 255;
            bi = b * 5 / 255;
            indexed[i] = (GifByteType)(ri * 36 + gi * 6 + bi);
        }
    }

    cmap = GifMakeMapObject(color_count, palette);
    free(palette);
    palette = NULL;
    if (!cmap) {
        fprintf(stderr, "encode gif: color map creation failed\n");
        free(indexed);
        return 0;
    }

    gif = EGifOpenFileName(path, false, &err);
    if (!gif) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        GifFreeMapObject(cmap);
        free(indexed);
        return 0;
    }

    if (EGifPutScreenDesc(gif, img->width, img->height, 8, 0, cmap) == GIF_ERROR ||
        EGifPutImageDesc(gif, 0, 0, img->width, img->height, false, NULL) == GIF_ERROR) {
        fprintf(stderr, "encode gif: write header failed\n");
        EGifCloseFile(gif, &err);
        free(indexed);
        return 0;
    }

    for (y = 0; y < img->height; ++y) {
        if (EGifPutLine(gif, indexed + (size_t)y * (size_t)img->width, img->width) == GIF_ERROR) {
            fprintf(stderr, "encode gif: write pixels failed\n");
            EGifCloseFile(gif, &err);
            free(indexed);
            return 0;
        }
    }

    EGifCloseFile(gif, &err);
    free(indexed);
    return 1;
}

static int decode_heic(const char *path, Image *out) {
    struct heif_context *ctx = heif_context_alloc();
    struct heif_image_handle *handle = NULL;
    struct heif_image *image = NULL;
    struct heif_decoding_options *opts = NULL;
    struct heif_error err;
    int width;
    int height;
    int stride = 0;
    const uint8_t *plane = NULL;
    int y;

    if (!ctx) {
        fprintf(stderr, "decode heic: context allocation failed\n");
        return 0;
    }

    err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code != heif_error_Ok) {
        print_heif_error("decode heic", err);
        heif_context_free(ctx);
        return 0;
    }

    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) {
        print_heif_error("decode heic", err);
        heif_context_free(ctx);
        return 0;
    }

    width = heif_image_handle_get_width(handle);
    height = heif_image_handle_get_height(handle);

    opts = heif_decoding_options_alloc();
    err = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, opts);
    heif_decoding_options_free(opts);
    if (err.code != heif_error_Ok) {
        print_heif_error("decode heic", err);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return 0;
    }

    plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
    if (!plane || stride <= 0) {
        fprintf(stderr, "decode heic: missing pixel plane\n");
        heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return 0;
    }

    out->width = width;
    out->height = height;
    out->rgba = malloc((size_t)width * (size_t)height * 4u);
    if (!out->rgba) {
        fprintf(stderr, "decode heic: out of memory\n");
        heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return 0;
    }

    for (y = 0; y < height; ++y) {
        memcpy(
            out->rgba + (size_t)y * (size_t)width * 4u,
            plane + (size_t)y * (size_t)stride,
            (size_t)width * 4u
        );
    }

    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return 1;
}

static int encode_heic(const char *path, const Image *img, int quality, int shrink) {
    struct heif_context *ctx = NULL;
    struct heif_image *image = NULL;
    struct heif_encoder *encoder = NULL;
    struct heif_encoding_options *opts = NULL;
    struct heif_error err;
    uint8_t *plane;
    int stride = 0;
    int y;
    int effective_quality = quality;

    if (shrink && effective_quality > 70) {
        effective_quality = 70;
    }

    ctx = heif_context_alloc();
    if (!ctx) {
        fprintf(stderr, "encode heic: context allocation failed\n");
        return 0;
    }

    err = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
    if (err.code != heif_error_Ok) {
        print_heif_error("encode heic", err);
        heif_context_free(ctx);
        return 0;
    }

    heif_encoder_set_lossy_quality(encoder, effective_quality);

    err = heif_image_create(img->width, img->height, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, &image);
    if (err.code != heif_error_Ok) {
        print_heif_error("encode heic", err);
        heif_encoder_release(encoder);
        heif_context_free(ctx);
        return 0;
    }

    err = heif_image_add_plane(image, heif_channel_interleaved, img->width, img->height, 32);
    if (err.code != heif_error_Ok) {
        print_heif_error("encode heic", err);
        heif_image_release(image);
        heif_encoder_release(encoder);
        heif_context_free(ctx);
        return 0;
    }

    plane = heif_image_get_plane(image, heif_channel_interleaved, &stride);
    if (!plane || stride <= 0) {
        fprintf(stderr, "encode heic: pixel plane allocation failed\n");
        heif_image_release(image);
        heif_encoder_release(encoder);
        heif_context_free(ctx);
        return 0;
    }

    for (y = 0; y < img->height; ++y) {
        memcpy(
            plane + (size_t)y * (size_t)stride,
            img->rgba + (size_t)y * (size_t)img->width * 4u,
            (size_t)img->width * 4u
        );
    }

    opts = heif_encoding_options_alloc();
    err = heif_context_encode_image(ctx, image, encoder, opts, NULL);
    heif_encoding_options_free(opts);
    if (err.code != heif_error_Ok) {
        print_heif_error("encode heic", err);
        heif_image_release(image);
        heif_encoder_release(encoder);
        heif_context_free(ctx);
        return 0;
    }

    err = heif_context_write_to_file(ctx, path);
    if (err.code != heif_error_Ok) {
        print_heif_error("encode heic", err);
        heif_image_release(image);
        heif_encoder_release(encoder);
        heif_context_free(ctx);
        return 0;
    }

    heif_image_release(image);
    heif_encoder_release(encoder);
    heif_context_free(ctx);
    return 1;
}

static int decode_image(const char *path, ImageFormat fmt, Image *out) {
    memset(out, 0, sizeof(*out));
    switch (fmt) {
        case FORMAT_JPEG:
            return decode_jpeg(path, out);
        case FORMAT_PNG:
            return decode_png(path, out);
        case FORMAT_WEBP:
            return decode_webp(path, out);
        case FORMAT_TIFF:
            return decode_tiff(path, out);
        case FORMAT_GIF:
            return decode_gif(path, out);
        case FORMAT_HEIC:
            return decode_heic(path, out);
        default:
            fprintf(stderr, "unsupported input format\n");
            return 0;
    }
}

static int encode_image(const char *path, ImageFormat fmt, const Image *img, int quality, int shrink) {
    switch (fmt) {
        case FORMAT_JPEG:
            return encode_jpeg(path, img, quality, shrink);
        case FORMAT_PNG:
            return encode_png(path, img, shrink);
        case FORMAT_WEBP:
            return encode_webp(path, img, quality, shrink);
        case FORMAT_TIFF:
            return encode_tiff(path, img, shrink);
        case FORMAT_GIF:
            return encode_gif(path, img);
        case FORMAT_HEIC:
            return encode_heic(path, img, quality, shrink);
        default:
            fprintf(stderr, "unsupported output format\n");
            return 0;
    }
}

int main(int argc, char *argv[]) {
    int i = 1;
    int quality = 90;
    int shrink = 0;
    int resize_requested = 0;
    int resize_width = 0;
    int resize_height = 0;
    const char *input_path;
    const char *output_path;
    ImageFormat input_fmt;
    ImageFormat output_fmt;
    Image img;
    Image final_img;
    Image *img_to_write = NULL;

    memset(&img, 0, sizeof(img));
    memset(&final_img, 0, sizeof(final_img));

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quality") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[i]);
                return 2;
            }
            if (!parse_quality(argv[i + 1], &quality)) {
                fprintf(stderr, "invalid quality: %s\n", argv[i + 1]);
                return 2;
            }
            i += 2;
            continue;
        }

        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resize") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", argv[i]);
                return 2;
            }
            if (!parse_resize(argv[i + 1], &resize_width, &resize_height)) {
                fprintf(stderr, "invalid resize value: %s\n", argv[i + 1]);
                return 2;
            }
            resize_requested = 1;
            i += 2;
            continue;
        }

        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--shrink") == 0) {
            shrink = 1;
            i += 1;
            continue;
        }

        fprintf(stderr, "unknown option: %s\n", argv[i]);
        return 2;
    }

    if (argc - i != 2) {
        print_usage(argv[0]);
        return 2;
    }

    input_path = argv[i];
    output_path = argv[i + 1];
    input_fmt = format_from_path(input_path);
    output_fmt = format_from_path(output_path);

    if (input_fmt == FORMAT_UNKNOWN) {
        fprintf(stderr, "unsupported input extension\n");
        return 1;
    }
    if (output_fmt == FORMAT_UNKNOWN) {
        fprintf(stderr, "unsupported output extension\n");
        return 1;
    }

    if (!decode_image(input_path, input_fmt, &img)) {
        return 1;
    }

    if (resize_requested) {
        if (!resize_image(&img, &final_img, resize_width, resize_height)) {
            image_free(&img);
            return 1;
        }
        img_to_write = &final_img;
    } else {
        img_to_write = &img;
    }

    if (!encode_image(output_path, output_fmt, img_to_write, quality, shrink)) {
        image_free(&final_img);
        image_free(&img);
        return 1;
    }

    image_free(&final_img);
    image_free(&img);
    return 0;
}
