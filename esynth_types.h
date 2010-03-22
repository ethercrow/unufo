#ifndef ESYNTH_TYPES_H
#define ESYNTH_TYPES_H

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

typedef guint8 Pixelel;

typedef struct Coordinates {
    int x, y;
    Coordinates(int _x,int _y): x(_x), y(_y) { }
    Coordinates() { }

    bool operator < (const Coordinates& other) const { 
        return y*y+x*x < (other.y*other.y+other.x*other.x); 
    }

    Coordinates operator + (const Coordinates& a) const { 
        return Coordinates(x+a.x,y+a.y); 
    }

    Coordinates operator - (const Coordinates& a) const { 
        return Coordinates(x-a.x,y-a.y); 
    }
} Coordinates;

struct Parameters {
    bool h_tile, v_tile;
    bool use_border;
    bool invent_gradients;
    bool equal_adjustment;
    bool use_ref_layer;

    gint32 corpus_id, input_map_id, output_map_id;
    gint32 ref_layer_id;

    double map_weight;
    double autism;
    gint32 neighbours, tries;
    gint32 comp_size, transfer_size;
    gint32 max_adjustment;
};

//Bitmap class with three dimensions (width, height, number of channels)
template<class t>
struct Bitmap {
    int width, height, depth;
    t *data;

    Bitmap() { 
        data = 0;
    }

    ~Bitmap() {
        delete[] data;
    }

    void size(int w,int h,int d) {
        width = w; 
        height = h; 
        depth = d;

        delete[] data;
        data = new t[w*h*4];
    }

    t *at(int x,int y) const {
        return data + (y*width+x)*4;
    }

    t *at(const Coordinates position) const {
        return at(position.x,position.y);
    }


    void to_drawable(GimpDrawable *drawable, int x1,int y1, int src_layer) {
        int i,j;
        GimpPixelRgn region;
        guchar *img;

        gimp_pixel_rgn_init(&region, drawable, x1,y1,width,height, TRUE,TRUE);

        img = new guchar[width*height*depth];

        for(i=0;i<width*height;i++)
            for(j=0;j<int(drawable->bpp);j++)
                img[i*drawable->bpp+j] = data[i*4+src_layer+j];

        gimp_pixel_rgn_set_rect(&region, img, x1,y1,width,height);

        delete[] img;
    }

    void from_drawable(GimpDrawable *drawable, 
            int x1,int y1, int dest_layer) {
        int i,j;
        GimpPixelRgn region;
        guchar *img;

        gimp_pixel_rgn_init(&region, drawable, x1,y1,width,height, FALSE,FALSE);

        img = new guchar[width*height*depth];
        gimp_pixel_rgn_get_rect(&region, img, x1,y1,width,height);

        for(i=0;i<width*height;i++)
            for(j=0;j<(int)(drawable->bpp);j++)
                data[i*4+dest_layer+j] = img[i*drawable->bpp+j];

        delete[] img;
    }
};

#endif // ESYNTH_TYPES_H

