#pragma once
#include <stdint.h>

typedef int FT_Int32;
typedef signed long FT_Long, FT_Pos;
typedef unsigned long FT_ULong;
typedef unsigned int FT_UInt;
typedef unsigned char FT_Byte;
typedef int FT_Error;
typedef struct FT_LibraryRec_ *FT_Library;

typedef struct { FT_Pos x, y; } FT_Vector;
typedef struct { FT_Pos xScale, yScale; } FT_Matrix;
typedef struct {
    short n_contours, n_points;
    FT_Vector *points;
    unsigned char *tags, *contours;
    int flags;
} FT_Outline;

typedef struct {
    unsigned int rows, width, pitch;
    unsigned char *buffer;
    unsigned short num_grays;
    unsigned char pixel_mode, palette_mode;
    void *palette;
} FT_Bitmap;

typedef struct {
    int format;
    FT_Outline outline;
    struct { FT_Pos width, height, horiBearingX, horiBearingY, horiAdvance; } metrics;
    FT_Bitmap bitmap;
    int bitmap_left, bitmap_top;
    FT_Vector advance;
} FT_GlyphSlotRec;
typedef FT_GlyphSlotRec *FT_GlyphSlot;

typedef struct {
    FT_Pos ascender, descender, height, max_advance_width, max_advance_height, y_scale, x_scale;
} FT_Size_Metrics;
typedef struct { FT_Size_Metrics metrics; } FT_SizeRec;
typedef FT_SizeRec *FT_Size;

typedef struct FT_FaceRec_ {
    FT_Long num_faces, face_index, face_flags, style_flags, num_glyphs;
    FT_Long num_fixed_sizes;
    FT_UInt units_per_EM;
    FT_Pos ascender, descender, height, max_advance_width;
    FT_GlyphSlot glyph;
    FT_Size size;
} FT_FaceRec;
typedef FT_FaceRec *FT_Face;

typedef enum { FT_SIZE_REQUEST_TYPE_NOMINAL, FT_SIZE_REQUEST_TYPE_CELL } FT_Size_Request_Type;
typedef struct {
    FT_Size_Request_Type type;
    FT_Long width, height, horz_resolution, vert_resolution;
} FT_Size_RequestRec, *FT_Size_Request;

enum { FT_LOAD_NO_BITMAP = 8, FT_LOAD_TARGET_NORMAL = 0, FT_LOAD_TARGET_MONO = 1 << 16,
       FT_GLYPH_FORMAT_OUTLINE = 0x4F55544C, FT_PIXEL_MODE_MONO = 1,
       FT_RENDER_MODE_NORMAL = 0, FT_RENDER_MODE_MONO = 1 };
typedef unsigned int FT_Render_Mode;

FT_Error FT_Init_FreeType(FT_Library *alibrary);
FT_Error FT_Done_FreeType(FT_Library library);
FT_Error FT_New_Face(FT_Library, const char *, FT_Long, FT_Face *);
FT_Error FT_New_Memory_Face(FT_Library, const FT_Byte *, FT_Long, FT_Long, FT_Face *);
FT_Error FT_Done_Face(FT_Face);
FT_Error FT_Request_Size(FT_Face, FT_Size_Request);
FT_UInt FT_Get_Char_Index(FT_Face, FT_ULong);
FT_Error FT_Load_Glyph(FT_Face, FT_UInt, FT_Int32);
FT_Error FT_Render_Glyph(FT_GlyphSlot, FT_Render_Mode);
FT_Error FT_Outline_Embolden(FT_Outline *, FT_Pos);
FT_Long FT_MulFix(FT_Long, FT_Long);
