#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PSF font format converter for C-OS
 * Converts PSF v1 or PSF v2 fonts to C-OS font format
 */

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[2];      /* Magic bytes */
    uint8_t mode;          /* PSF mode */
    uint8_t charsize;      /* Character size in bytes */
} PSF1Header;

typedef struct {
    uint8_t magic[4];      /* Magic bytes */
    uint32_t version;      /* Version */
    uint32_t headersize;   /* Header size */
    uint32_t flags;        /* Flags */
    uint32_t length;       /* Number of glyphs */
    uint32_t charsize;     /* Character size in bytes */
    uint32_t height;       /* Height in pixels */
    uint32_t width;        /* Width in pixels */
} PSF2Header;
#pragma pack(pop)

typedef struct {
    uint16_t codepoint;
    uint32_t rows[16];
} vga_font24_glyph_t;

/* Convert 8x16 PSF glyph to 12x16 C-OS glyph */
void convert_8x16_to_12x16(const uint8_t* src, uint32_t* dst) {
    for (int row = 0; row < 16; row++) {
        uint32_t bits = 0;
        uint8_t src_byte = src[row];
        
        /* Convert 8-bit row to 12-bit row, centered */
        /* PSF: bit 7 is leftmost, bit 0 is rightmost */
        /* C-OS: bit 0 is leftmost, bit 11 is rightmost (based on current rendering logic) */
        
        for (int col = 0; col < 8; col++) {
            if (src_byte & (1u << (7 - col))) {
                /* Center the 8-pixel glyph in 12-pixel width */
                bits |= (1u << (col + 2));  /* Shift by 2 to center */
            }
        }
        
        dst[row] = bits;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <input.psf> <output.c>\n", argv[0]);
        return 1;
    }
    
    const char* input_file = argv[1];
    const char* output_file = argv[2];
    
    FILE* fp = fopen(input_file, "rb");
    if (!fp) {
        printf("Error: Could not open input file %s\n", input_file);
        return 1;
    }
    
    /* Read header to determine PSF version */
    uint8_t magic[4];
    fread(magic, 1, 4, fp);
    rewind(fp);
    
    int is_psf2 = (magic[0] == 0x72 && magic[1] == 0x62 && magic[2] == 0x73 && magic[3] == 0x02);
    
    int num_glyphs = 256;
    int charsize = 16;
    int width = 8;
    int height = 16;
    
    if (is_psf2) {
        PSF2Header header;
        fread(&header, sizeof(PSF2Header), 1, fp);
        num_glyphs = header.length;
        charsize = header.charsize;
        height = header.height;
        width = header.width;
        printf("PSF v2: %d glyphs, %dx%d pixels, %d bytes per glyph\n", 
               num_glyphs, width, height, charsize);
    } else {
        PSF1Header header;
        fread(&header, sizeof(PSF1Header), 1, fp);
        num_glyphs = (header.mode & 0x01) ? 512 : 256;
        charsize = header.charsize;
        height = charsize;
        width = 8;
        printf("PSF v1: %d glyphs, %dx%d pixels, %d bytes per glyph\n", 
               num_glyphs, width, height, charsize);
    }
    
    /* Read glyph data */
    uint8_t* glyph_data = (uint8_t*)malloc(num_glyphs * charsize);
    if (!glyph_data) {
        printf("Error: Could not allocate memory for glyph data\n");
        fclose(fp);
        return 1;
    }
    
    fread(glyph_data, 1, num_glyphs * charsize, fp);
    fclose(fp);
    
    /* Write output file */
    FILE* out = fopen(output_file, "w");
    if (!out) {
        printf("Error: Could not open output file %s\n", output_file);
        free(glyph_data);
        return 1;
    }
    
    fprintf(out, "#include \"vga_font24.h\"\n");
    fprintf(out, "#include \"font24x24.h\"\n");
    fprintf(out, "\n");
    fprintf(out, "/*\n");
    fprintf(out, " * Font converted from %s\n", input_file);
    fprintf(out, " * PSF format: %dx%d pixels, %d glyphs\n", width, height, num_glyphs);
    fprintf(out, " * Converted to 12x16 for C-OS\n");
    fprintf(out, " */\n");
    fprintf(out, "\n");
    fprintf(out, "const vga_font24_glyph_t font24x24[] = {\n");
    
    /* Convert and write glyphs (ASCII 0x20-0x7E) */
    for (int i = 0; i < num_glyphs && i < 0x7F; i++) {
        if (i < 0x20) {
            /* Skip control characters, use empty glyph */
            fprintf(out, "    {0x%04xu, {", i);
            for (int row = 0; row < 16; row++) {
                fprintf(out, "0x0000u");
                if (row < 15) fprintf(out, ",");
            }
            fprintf(out, "}},\n");
        } else {
            uint32_t rows[16];
            const uint8_t* src_glyph = glyph_data + i * charsize;
            
            if (height == 16 && width == 8) {
                convert_8x16_to_12x16(src_glyph, rows);
            } else {
                /* For other sizes, use simple pattern */
                for (int row = 0; row < 16; row++) {
                    rows[row] = 0;
                    if (row < height && i >= 0x20) {
                        for (int col = 0; col < width && col < 12; col++) {
                            int byte_idx = row * ((width + 7) / 8) + col / 8;
                            int bit_idx = 7 - (col % 8);
                            if (src_glyph[byte_idx] & (1u << bit_idx)) {
                                rows[row] |= (1u << col);
                            }
                        }
                    }
                }
            }
            
            fprintf(out, "    {0x%04xu, {", i);
            for (int row = 0; row < 16; row++) {
                fprintf(out, "0x%04xu", rows[row]);
                if (row < 15) fprintf(out, ",");
            }
            fprintf(out, "}},\n");
        }
    }
    
    fprintf(out, "};\n");
    fclose(out);
    
    free(glyph_data);
    
    printf("Font data written to %s\n", output_file);
    return 0;
}
