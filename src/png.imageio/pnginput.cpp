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

#include <boost/algorithm/string.hpp>
using boost::algorithm::iequals;
#include <ImathColor.h>

#include <png.h>

#include "dassert.h"
#include "paramtype.h"
#include "imageio.h"
#include "thread.h"
#include "strutil.h"
#include "fmath.h"

using namespace OpenImageIO;


// FIXME -- these test images don't work properly: 
//   basi3p?? -- 1, 2, 4, 8 bit paletted
//   basn4a{08,16} -- 8 and 16 bit graycale with alpha
// 


class PNGInput : public ImageInput {
public:
    PNGInput () { init(); }
    virtual ~PNGInput () { close(); }
    virtual const char * format_name (void) const { return "png"; }
    virtual bool open (const char *name, ImageSpec &newspec);
    virtual bool close ();
    virtual int current_subimage (void) const { return m_subimage; }
    virtual bool seek_subimage (int index, ImageSpec &newspec);
    virtual bool read_native_scanline (int y, int z, void *data);

private:
    std::string m_filename;           ///< Stash the filename
    FILE *m_file;                     ///< Open image handle
    png_structp m_png;                ///< PNG read structure pointer
    png_infop m_info;                 ///< PNG image info structure pointer
    int m_bit_depth;                  ///< PNG bit depth
    int m_color_type;                 ///< PNG color model type
    std::vector<unsigned char> m_buf; ///< Buffer the image pixels
    int m_subimage;                   ///< What subimage are we looking at?
    Imath::Color3f m_bg;              ///< Background color

    /// Reset everything to initial state
    ///
    void init () {
        m_subimage = -1;
        m_file = NULL;
        m_png = NULL;
        m_info = NULL;
        m_buf.clear ();
    }

    /// Helper function: read the image.
    ///
    bool readimg ();

    /// Extract the background color.
    ///
    bool get_background (float *red, float *green, float *blue);
};



// Obligatory material to make this a recognizeable imageio plugin:
extern "C" {

DLLEXPORT PNGInput *png_input_imageio_create () { return new PNGInput; }

DLLEXPORT int imageio_version = IMAGEIO_VERSION;

DLLEXPORT const char * png_input_extensions[] = {
    "png", NULL
};

};



// Someplace to store an error message from the PNG error handler
static std::string lasterr;
static mutex lasterr_mutex;


static void
my_error_handler (const char *str, const char *format, va_list ap)
{
    lock_guard lock (lasterr_mutex);
    lasterr = Strutil::vformat (format, ap);
}



bool
PNGInput::open (const char *name, ImageSpec &newspec)
{
    m_filename = name;
    m_subimage = 0;

    m_file = fopen (name, "rb");
    if (! m_file) {
        error ("Could not open file %s", name);
        return false;
    }

    unsigned char sig[8];
    fread (sig, 1, sizeof(sig), m_file);
    if (! png_check_sig (sig, sizeof(sig))) {
        error ("File failed PNG signature check");
        return false;
    }

    m_png = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (! m_png) {
        close ();
        error ("Could not create PNG read structure");
        return false;
    }

    m_info = png_create_info_struct (m_png);
    if (! m_info) {
        close ();
        error ("Could not create PNG info structure");
        return false;
    }

    // Must call this setjmp in every function that does PNG reads
    if (setjmp (png_jmpbuf(m_png))) {
        close ();
        error ("PNG library error");
        return false;
    }

    png_init_io (m_png, m_file);
    png_set_sig_bytes (m_png, 8);  // already read 8 bytes
    png_read_info (m_png, m_info);

    unsigned int width, height;
    png_get_IHDR (m_png, m_info,
                  (png_uint_32 *)&width, (png_uint_32 *)&height,
                  &m_bit_depth, &m_color_type, NULL, NULL, NULL);
    
    m_spec = ImageSpec ((int)width, (int)height,
                        png_get_channels (m_png, m_info),
                        m_bit_depth == 16 ? PT_UINT16 : PT_UINT8);

    m_spec.default_channel_names ();

    double gamma;
    if (png_get_gAMA (m_png, m_info, &gamma)) {
        m_spec.gamma = (float) gamma;
        m_spec.linearity = (gamma == 1) ? ImageSpec::Linear 
                                        : ImageSpec::GammaCorrected;
    }
    int srgb_intent;
    if (png_get_sRGB (m_png, m_info, &srgb_intent)) {
        m_spec.linearity = ImageSpec::sRGB;
    }
    png_timep mod_time;
    if (png_get_tIME (m_png, m_info, &mod_time)) {
        std::string date = Strutil::format ("%4d:%02d:%02d %2d:%02d:%02d",
                           mod_time->year, mod_time->month, mod_time->day,
                           mod_time->hour, mod_time->minute, mod_time->second);
        m_spec.attribute ("DateTime", date); 
    }
    png_color_16p background;
    if (png_get_bKGD (m_png, m_info, &background)) {
        // FIXME
    }
    
    png_textp text_ptr;
    int num_comments = png_get_text (m_png, m_info, &text_ptr, NULL);
    if (num_comments) {
        std::string comments;
        // FIXME
        for (int i = 0;  i < num_comments;  ++i) {
            if (iequals (text_ptr[i].key, "Description"))
                m_spec.attribute ("ImageDescription", text_ptr[i].text);
            else if (iequals (text_ptr[i].key, "Author"))
                m_spec.attribute ("Artist", text_ptr[i].text);
            else if (iequals (text_ptr[i].key, "Title"))
                m_spec.attribute ("DocumentName", text_ptr[i].text);
            else
                m_spec.attribute (text_ptr[i].key, text_ptr[i].text);
        }
    }
    m_spec.x = png_get_x_offset_pixels (m_png, m_info);
    m_spec.y = png_get_y_offset_pixels (m_png, m_info);

    int unit;
    double wscale, hscale;
    if (png_get_sCAL (m_png, m_info, &unit, &wscale, &hscale)) {
        if (unit == PNG_SCALE_METER)
            m_spec.attribute ("ResolutionUnit", "meter");
        else if (unit == PNG_SCALE_RADIAN)
            m_spec.attribute ("ResolutionUnit", "radian");
        else
            m_spec.attribute ("ResolutionUnit", "unknown");
        m_spec.attribute ("XResolution", (float)wscale);
        m_spec.attribute ("YResolution", (float)hscale);
    }

    float aspect = (float)png_get_pixel_aspect_ratio (m_png, m_info);
    if (aspect != 0 && aspect != 1)
        m_spec.attribute ("PixelAspectRatio", aspect);

    float r, g, b;
    if (get_background (&r, &g, &b)) {
        m_bg = Imath::Color3f (r, g, b);
        // FIXME -- should we be doing anything with this?
    }

    newspec = spec ();
    return true;
}



bool
PNGInput::seek_subimage (int index, ImageSpec &newspec)
{
    if (index == m_subimage) {
        newspec = spec();
        return true;
    } else {
        // PNG doesn't support multiple images
        return false;
    }
}



bool
PNGInput::get_background (float *red, float *green, float *blue)
{
    if (setjmp (png_jmpbuf (m_png)))
        return false;
    if (! png_get_valid (m_png, m_info, PNG_INFO_bKGD))
        return false;

    png_color_16p bg;
    png_get_bKGD (m_png, m_info, &bg);
    if (spec().format == PT_UINT16) {
        *red   = bg->red   / 65535.0;
        *green = bg->green / 65535.0;
        *blue  = bg->blue  / 65535.0;
    } else if (spec().nchannels < 3 && m_bit_depth < 8) {
        if (m_bit_depth == 1)
            *red = *green = *blue = (bg->gray ? 1 : 0);
        else if (m_bit_depth == 2)
            *red = *green = *blue = bg->gray / 3.0;
        else // 4 bits
            *red = *green = *blue = bg->gray / 15.0;
    } else {
        *red   = bg->red   / 255.0;
        *green = bg->green / 255.0;
        *blue  = bg->blue  / 255.0;
    }
    return true;
}



bool
PNGInput::readimg ()
{
    // Must call this setjmp in every function that does PNG reads
    if (setjmp (png_jmpbuf (m_png))) {
        close ();
        error ("PNG library error");
        return false;
    }

    // Auto-convert palette images to RGB
    if (m_color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb (m_png);
    // Auto-convert 1-, 2-, and 4- bit grayscale to 8 bits
    if (m_color_type == PNG_COLOR_TYPE_GRAY && m_bit_depth < 8)
        png_set_gray_1_2_4_to_8 (m_png);
    // Auto-convert transparency to alpha
    if (png_get_valid (m_png, m_info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha (m_png);
    // Make the library handle fewer significant bits
    png_color_8p sig_bit;
    if (png_get_sBIT (m_png, m_info, &sig_bit))
        png_set_shift (m_png, sig_bit);
    // PNG files are naturally big-endian
    if (littleendian())
        png_set_swap (m_png);

    double gamma;
    if (png_get_gAMA (m_png, m_info, &gamma))
        png_set_gamma (m_png, 1.0, gamma);

    png_read_update_info (m_png, m_info);

    DASSERT (m_spec.scanline_bytes() == png_get_rowbytes(m_png,m_info));
    m_buf.resize (m_spec.image_bytes());

    std::vector<unsigned char *> row_pointers (m_spec.height);
    for (int i = 0;  i < m_spec.height;  ++i)
        row_pointers[i] = &m_buf[0] + i * m_spec.scanline_bytes();;

    png_read_image (m_png, &row_pointers[0]);
    png_read_end (m_png, NULL);

    return true;
}



bool
PNGInput::close ()
{
    if (m_png && m_info) {
        png_destroy_read_struct (&m_png, &m_info, NULL);
        m_png = NULL;
        m_info = NULL;
    }
    if (m_file) {
        fclose (m_file);
        m_file = NULL;
    }

    init();  // Reset to initial state
    return true;
}



bool
PNGInput::read_native_scanline (int y, int z, void *data)
{
    if (m_buf.empty ())
        readimg ();

    y -= m_spec.y;
    size_t size = spec().scanline_bytes();
    memcpy (data, &m_buf[0] + y * size, size);
    return true;
}