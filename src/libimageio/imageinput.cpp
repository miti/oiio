/////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2008 Larry Gritz
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// 
// (this is the MIT license)
/////////////////////////////////////////////////////////////////////////////


#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <unistd.h>

#include "dassert.h"
#include "paramtype.h"
#include "strutil.h"

#define DLL_EXPORT_PUBLIC /* Because we are implementing ImageIO */
#include "imageio.h"
#include "imageio_pvt.h"
#undef DLL_EXPORT_PUBLIC

using namespace OpenImageIO;
using namespace OpenImageIO::pvt;



bool 
ImageInput::read_scanline (int y, int z, ParamBaseType format, void *data,
                           stride_t xstride)
{
    m_spec.auto_stride (xstride);
    bool contiguous = (xstride == m_spec.nchannels*ParamBaseTypeSize(format));
    if (contiguous && m_spec.format == format)  // Simple case
        return read_native_scanline (y, z, data);

    // Complex case -- either changing data type or stride
    int scanline_values = m_spec.width * m_spec.nchannels;
    unsigned char *buf = (unsigned char *) alloca (m_spec.scanline_bytes());
    bool ok = read_native_scanline (y, z, buf);
    if (! ok)
        return false;
    ok = contiguous 
        ? convert_types (m_spec.format, buf, format, data, scanline_values)
        : convert_image (m_spec.nchannels, m_spec.width, 1, 1, 
                         buf, m_spec.format, AutoStride, AutoStride, AutoStride,
                         data, format, xstride, AutoStride, AutoStride);
    if (! ok)
        error ("ImageInput::read_scanline : no support for format %s",
               ParamBaseTypeNameString(m_spec.format));
    return ok;
}



bool 
ImageInput::read_tile (int x, int y, int z, ParamBaseType format, void *data,
                       stride_t xstride, stride_t ystride, stride_t zstride)
{
    m_spec.auto_stride (xstride, ystride, zstride);
    bool contiguous = (xstride == m_spec.nchannels*ParamBaseTypeSize(format) &&
                       ystride == xstride*m_spec.tile_width &&
                       zstride == ystride*m_spec.tile_height);
    if (contiguous && m_spec.format == format)  // Simple case
        return read_native_tile (x, y, z, data);

    // Complex case -- either changing data type or stride
    int tile_values = m_spec.tile_width * m_spec.tile_height * 
                      m_spec.tile_depth * m_spec.nchannels;
    unsigned char *buf = (unsigned char *) alloca (m_spec.tile_bytes());
    bool ok = read_native_tile (x, y, z, buf);
    if (! ok)
        return false;
    ok = contiguous 
        ? convert_types (m_spec.format, buf, format, data, tile_values)
        : convert_image (m_spec.nchannels, m_spec.tile_width, m_spec.tile_height, m_spec.tile_depth, 
                         buf, m_spec.format, AutoStride, AutoStride, AutoStride,
                         data, format, xstride, ystride, zstride);
    if (! ok)
        error ("ImageInput::read_tile : no support for format %s",
               ParamBaseTypeNameString(m_spec.format));
    return ok;

}



bool
ImageInput::read_image (ParamBaseType format, void *data,
                        stride_t xstride, stride_t ystride, stride_t zstride,
                        OpenImageIO::ProgressCallback progress_callback,
                        void *progress_callback_data)
{
    m_spec.auto_stride (xstride, ystride, zstride);
    bool ok = true;
    if (progress_callback)
        if (progress_callback (progress_callback_data, 0.0f))
            return ok;
    if (m_spec.tile_width) {
        // Tiled image

        // FIXME: what happens if the image dimensions are smaller than
        // the tile dimensions?  Or if one of the tiles runs past the
        // right or bottom edge?  Do we need to allocate a full tile and
        // copy into it into buf?  That's probably the safe thing to do.
        // Or should that handling be pushed all the way into read_tile
        // itself?
        for (int z = 0;  z < m_spec.depth;  z += m_spec.tile_depth)
            for (int y = 0;  y < m_spec.height;  y += m_spec.tile_height) {
                for (int x = 0;  x < m_spec.width && ok;  x += m_spec.tile_width)
                    ok &= read_tile (x, y, z, format,
                                     (char *)data + z*zstride + y*ystride + x*xstride,
                                     xstride, ystride, zstride);
                if (progress_callback)
                    if (progress_callback (progress_callback_data, (float)y/m_spec.height))
                        return ok;
            }
    } else {
        // Scanline image
        for (int z = 0;  z < m_spec.depth;  ++z)
            for (int y = 0;  y < m_spec.height && ok;  ++y) {
                ok &= read_scanline (y, z, format,
                                     (char *)data + z*zstride + y*ystride,
                                     xstride);
                if (progress_callback && !(y & 0x0f))
                    if (progress_callback (progress_callback_data, (float)y/m_spec.height))
                        return ok;
            }
    }
    if (progress_callback)
        progress_callback (progress_callback_data, 1.0f);
    return ok;
}



int 
ImageInput::send_to_input (const char *format, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



int 
ImageInput::send_to_client (const char *format, ...)
{
    // FIXME -- I can't remember how this is supposed to work
    return 0;
}



void 
ImageInput::error (const char *message, ...)
{
    va_list ap;
    va_start (ap, message);
    m_errmessage = Strutil::vformat (message, ap);
    va_end (ap);
}
